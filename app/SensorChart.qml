import QtQuick

// Gráfico no tempo da temperatura e/ou umidade de uma placa.
//
// "both" empilha uma faixa de temperatura sobre uma de umidade, com o mesmo
// eixo de tempo e uma só mira, para comparar momento a momento enquanto cada
// uma mantém a própria escala (duas escalas no mesmo gráfico tornariam os
// cruzamentos e as alturas relativas das linhas sem sentido).
//
// collapseGaps junta os trechos com leituras: cada período sem dados (placa
// desligada ou fora de alcance) vira uma faixa estreita, para que dias de
// placa parada não espremam as horas medidas num canto do gráfico.
//
// showOutside acrescenta à faixa de temperatura a temperatura externa
// (outsidePoints, de hora em hora), tracejada em cinza: é a referência, não
// uma medida da colmeia. Ela entra na escala da faixa, então a linha da
// colmeia fica mais achatada, mas a distância entre as duas é o que importa.
Item {
    id: root

    property var points: []          // [{t: ms desde a época, temp, hum}], do mais antigo ao mais novo
    property real bucketMs: 60000    // largura de um ponto (ver gapMs)
    property real startMs: 0         // faixa do eixo x
    property real endMs: 0
    property string mode: "both"     // "both" | "temp" | "hum"
    property bool collapseGaps: false
    property var outsidePoints: []   // [{t, temp}], do mais antigo ao mais novo
    property bool showOutside: false

    readonly property color tempColor: Theme.propolis
    readonly property color humColor: Theme.sky
    readonly property color outsideColor: Theme.outside
    readonly property color gridColor: Theme.rule
    readonly property color axisTextColor: Theme.inkSoft
    readonly property color surfaceColor: Theme.paper
    // O texto do Canvas não segue a fonte do app: ela é informada aqui.
    readonly property string canvasFont: "12px '" + Theme.fontFamily + "'"
    readonly property string canvasTitleFont: "bold 13px '" + Theme.fontFamily + "'"

    // Geometria do gráfico (usada pelo canvas e pela camada de mira).
    readonly property real padLeft: 44
    readonly property real padRight: 14
    readonly property real padTop: 4
    readonly property real padBottom: 26
    readonly property real bandTitleHeight: 22
    readonly property real bandGap: 14

    property int hoverIndex: -1

    // Pontos mais distantes que isto são um período sem dados (placa desligada
    // ou fora de alcance): a linha é interrompida em vez de ligá-los. Medido
    // pelo espaçamento dos próprios dados, já que as placas gravam a cada 1 a 60 min.
    readonly property real gapMs: Math.max(bucketMs * 3, typicalStepMs * 3, 60000)

    // Mediana do tempo entre pontos consecutivos.
    readonly property real typicalStepMs: {
        if (points.length < 2)
            return 0
        const steps = []
        for (let i = 1; i < points.length; i++)
            steps.push(points[i].t - points[i - 1].t)
        steps.sort((a, b) => a - b)
        return steps[Math.floor(steps.length / 2)]
    }

    // Trechos contínuos de leituras, [{start, end}], separados por mais de gapMs.
    readonly property var runs: {
        const result = []
        for (const p of points) {
            const run = result.length > 0 ? result[result.length - 1] : null
            if (run && p.t - run.end <= gapMs)
                run.end = p.t
            else
                result.push({ start: p.t, end: p.t })
        }
        return result
    }
    readonly property int gapCount: Math.max(0, runs.length - 1)
    readonly property real hiddenMs: {
        let total = 0
        for (let i = 1; i < runs.length; i++)
            total += runs[i].start - runs[i - 1].end
        return total
    }
    readonly property bool collapsed: collapseGaps && gapCount > 0

    // Com collapsed, cada trecho ocupa largura proporcional à sua duração (na
    // mesma escala para todos) e cada período sem dados, gapPx. Um trecho de
    // um só ponto conta como um passo, para não ter largura zero.
    readonly property real gapPx: collapsed ? Math.min(16, (width - padLeft - padRight) * 0.25 / gapCount) : 0
    readonly property var runLayout: {
        if (!collapsed)
            return null
        const minDur = Math.max(bucketMs, typicalStepMs, 1)
        let total = 0
        for (const r of runs)
            total += Math.max(r.end - r.start, minDur)
        const pxPerMs = (width - padLeft - padRight - gapPx * gapCount) / total
        const items = []
        let x = padLeft
        for (const r of runs) {
            const dur = r.end - r.start
            const eff = Math.max(dur, minDur)
            // (eff - dur) / 2 centraliza o trecho curto no espaço reservado.
            items.push({ start: r.start, end: r.end, pad: (eff - dur) / 2, x0: x, x1: x + eff * pxPerMs })
            x += eff * pxPerMs + gapPx
        }
        return { pxPerMs: pxPerMs, items: items }
    }

    readonly property var bands: mode === "both" ? ["temp", "hum"] : [mode]

    // Horas externas mais distantes que isto não são ligadas (falta no serviço).
    readonly property real outsideGapMs: 3 * 3600000

    // Temperatura externa no instante t, interpolada entre as horas vizinhas;
    // null fora dos dados ou num buraco deles.
    function outsideAt(t) {
        const pts = outsidePoints
        if (pts.length === 0 || t < pts[0].t - 1800000 || t > pts[pts.length - 1].t + 1800000)
            return null
        let lo = 0, hi = pts.length - 1
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (pts[mid].t <= t) lo = mid; else hi = mid - 1
        }
        const a = pts[lo]
        if (t <= a.t || lo === pts.length - 1)
            return Math.abs(t - a.t) <= 1800000 ? a.temp : null
        const b = pts[lo + 1]
        if (b.t - a.t > outsideGapMs)
            return null
        return a.temp + (b.temp - a.temp) * (t - a.t) / (b.t - a.t)
    }

    // Trechos da linha externa que aparecem no gráfico: [[{t, temp}]]. Cada
    // janela (o eixo todo, ou cada trecho com leituras quando os períodos sem
    // dados estão juntados) começa e termina no valor interpolado da borda.
    readonly property var outsideSegments: {
        if (!showOutside || outsidePoints.length === 0 || mode === "hum")
            return []
        const windows = runLayout ? runs.filter(r => r.end > r.start).map(r => [r.start, r.end])
                                  : [[startMs, endMs]]
        const segments = []
        for (const w of windows) {
            let current = []
            const push = (t, temp) => {
                if (temp === null) {
                    if (current.length > 1) segments.push(current)
                    current = []
                    return
                }
                const prev = current.length > 0 ? current[current.length - 1] : null
                if (prev && t - prev.t > outsideGapMs) {
                    if (current.length > 1) segments.push(current)
                    current = []
                }
                current.push({ t: t, temp: temp })
            }
            push(w[0], outsideAt(w[0]))
            for (const p of outsidePoints) {
                if (p.t > w[0] && p.t < w[1])
                    push(p.t, p.temp)
            }
            push(w[1], outsideAt(w[1]))
            if (current.length > 1)
                segments.push(current)
        }
        return segments
    }

    function measure(key) {
        return key === "temp"
            ? { key: "temp", title: "Temperatura", unit: "°C", color: tempColor, decimals: 1 }
            : { key: "hum",  title: "Umidade",     unit: "%",  color: humColor,  decimals: 1 }
    }

    // Extensão vertical da faixa i: {top, bottom} da área do gráfico.
    function bandRect(i) {
        const usable = height - padTop - padBottom - (bands.length - 1) * bandGap
        const each = usable / bands.length
        const top = padTop + i * (each + bandGap)
        return { top: top + bandTitleHeight, bottom: top + each }
    }

    function xFor(t) {
        const layout = runLayout
        if (!layout) {
            const span = Math.max(1, endMs - startMs)
            return padLeft + (t - startMs) / span * (width - padLeft - padRight)
        }
        // Último trecho que começa até t: t cai nele ou no período sem dados seguinte.
        const items = layout.items
        let lo = 0, hi = items.length - 1
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (items[mid].start <= t) lo = mid; else hi = mid - 1
        }
        const r = items[lo]
        if (t <= r.start)
            return r.x0 + r.pad * layout.pxPerMs
        if (t <= r.end || lo === items.length - 1)
            return Math.min(r.x1, r.x0 + (t - r.start + r.pad) * layout.pxPerMs)
        const next = items[lo + 1]
        return r.x1 + (t - r.end) / (next.start - r.end) * gapPx
    }

    // Inverso de xFor para os rótulos do eixo; null num período sem dados juntado.
    function timeAt(x) {
        const layout = runLayout
        if (!layout)
            return startMs + (x - padLeft) / (width - padLeft - padRight) * (endMs - startMs)
        for (const r of layout.items) {
            if (x <= r.x1 + 0.5) {
                if (x < r.x0 - 0.5)
                    return null
                const t = r.start + (x - r.x0) / layout.pxPerMs - r.pad
                return Math.max(r.start, Math.min(r.end, t))
            }
        }
        return null
    }

    // Marcas redondas no eixo: ~4 passos de 1/2/5 x 10^n cobrindo os dados
    // (sem 2,5: os rótulos precisariam de mais uma casa, ex.: 62,5).
    function niceScale(minV, maxV) {
        if (minV === maxV) { minV -= 1; maxV += 1 }
        const rough = (maxV - minV) / 4
        const pow = Math.pow(10, Math.floor(Math.log10(rough)))
        let step = pow
        for (const f of [1, 2, 5, 10]) {
            if (f * pow >= rough) { step = f * pow; break }
        }
        const lo = Math.floor(minV / step) * step
        const hi = Math.ceil(maxV / step) * step
        const ticks = []
        for (let v = lo; v <= hi + step / 2; v += step)
            ticks.push(Math.round(v * 1000) / 1000)
        return { lo: lo, hi: hi, ticks: ticks, decimals: step < 1 ? 1 : 0 }
    }

    function scaleFor(key) {
        let minV = Infinity, maxV = -Infinity
        for (const p of points) {
            const v = p[key]
            if (v === undefined || v === null) continue
            minV = Math.min(minV, v)
            maxV = Math.max(maxV, v)
        }
        if (minV === Infinity)
            return null
        if (key === "temp") {
            for (const segment of outsideSegments) {
                for (const p of segment) {
                    minV = Math.min(minV, p.temp)
                    maxV = Math.max(maxV, p.temp)
                }
            }
        }
        return niceScale(minV, maxV)
    }

    function yFor(v, scale, rect) {
        return rect.bottom - (v - scale.lo) / (scale.hi - scale.lo) * (rect.bottom - rect.top)
    }

    function timeLabel(ms, withDate) {
        const spanH = (endMs - startMs) / 3600000
        const d = new Date(ms)
        if (withDate)
            return Qt.formatDateTime(d, "dd/MM/yyyy HH:mm:ss")
        if (spanH <= 0.25)
            return Qt.formatDateTime(d, "HH:mm:ss")
        if (spanH <= 36)
            return Qt.formatDateTime(d, "HH:mm")
        // Juntando os períodos sem dados, marcas vizinhas podem cair no mesmo dia.
        return Qt.formatDateTime(d, collapsed ? "dd/MM HH:mm" : "dd/MM")
    }

    // Números em pt-BR: vírgula decimal.
    function number(value, decimals) {
        return Number(value).toLocaleString(Qt.locale(), "f", decimals)
    }

    // Ponto mais próximo de x na tela (xFor cresce com o tempo, e os pontos
    // estão em ordem de tempo).
    function indexAt(x) {
        if (points.length === 0) return -1
        let lo = 0, hi = points.length - 1
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            if (xFor(points[mid].t) < x) lo = mid + 1; else hi = mid
        }
        if (lo > 0 && Math.abs(xFor(points[lo - 1].t) - x) < Math.abs(xFor(points[lo].t) - x))
            lo -= 1
        return lo
    }

    onPointsChanged: { hoverIndex = -1; canvas.requestPaint() }
    onModeChanged: canvas.requestPaint()
    onCollapsedChanged: canvas.requestPaint()
    onOutsideSegmentsChanged: canvas.requestPaint()
    onHoverIndexChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            if (root.points.length === 0)
                return

            const plotRight = width - root.padRight
            ctx.font = root.canvasFont

            // Linha da mira atravessando as faixas (por baixo dos pontos desenhados depois).
            if (root.hoverIndex >= 0) {
                const x = Math.round(root.xFor(root.points[root.hoverIndex].t)) + 0.5
                ctx.strokeStyle = Theme.inkFaint
                ctx.lineWidth = 1
                ctx.beginPath()
                ctx.moveTo(x, root.padTop)
                ctx.lineTo(x, height - root.padBottom)
                ctx.stroke()
            }

            for (let i = 0; i < root.bands.length; i++) {
                const m = root.measure(root.bands[i])
                const rect = root.bandRect(i)
                const scale = root.scaleFor(m.key)

                // Título da faixa com um traço na cor da série (a legenda).
                ctx.strokeStyle = m.color
                ctx.lineWidth = 2
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.moveTo(root.padLeft, rect.top - root.bandTitleHeight + 9)
                ctx.lineTo(root.padLeft + 14, rect.top - root.bandTitleHeight + 9)
                ctx.stroke()
                ctx.fillStyle = Theme.ink
                ctx.font = root.canvasTitleFont
                ctx.textAlign = "left"
                ctx.textBaseline = "middle"
                const titleText = m.title + " (" + m.unit + ")"
                ctx.fillText(titleText, root.padLeft + 20, rect.top - root.bandTitleHeight + 9)
                const titleEnd = root.padLeft + 20 + ctx.measureText(titleText).width
                ctx.font = root.canvasFont

                // Legenda da linha externa, ao lado do título.
                if (m.key === "temp" && root.outsideSegments.length > 0) {
                    const ly = rect.top - root.bandTitleHeight + 9
                    ctx.strokeStyle = root.outsideColor
                    ctx.lineWidth = 2
                    ctx.lineCap = "butt"
                    ctx.setLineDash([4, 3])
                    ctx.beginPath()
                    ctx.moveTo(titleEnd + 16, ly)
                    ctx.lineTo(titleEnd + 34, ly)
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.fillStyle = root.axisTextColor
                    ctx.fillText("Externa", titleEnd + 40, ly)
                }

                if (!scale) {
                    ctx.fillStyle = root.axisTextColor
                    ctx.fillText("Sem dados de " + m.title.toLowerCase(), root.padLeft, (rect.top + rect.bottom) / 2)
                    continue
                }

                // Grade discreta de linhas finas + rótulos do eixo y.
                ctx.lineWidth = 1
                ctx.strokeStyle = root.gridColor
                ctx.fillStyle = root.axisTextColor
                ctx.textAlign = "right"
                for (const tick of scale.ticks) {
                    const y = Math.round(root.yFor(tick, scale, rect)) + 0.5
                    ctx.beginPath()
                    ctx.moveTo(root.padLeft, y)
                    ctx.lineTo(plotRight, y)
                    ctx.stroke()
                    ctx.fillText(root.number(tick, scale.decimals), root.padLeft - 6, y)
                }

                // Períodos sem dados juntados: faixa clara cortando a grade, com
                // um zigue-zague de "eixo interrompido" no meio.
                if (root.runLayout) {
                    const items = root.runLayout.items
                    for (let g = 0; g + 1 < items.length; g++) {
                        const x0 = items[g].x1
                        const x1 = items[g + 1].x0
                        ctx.fillStyle = Theme.wing
                        ctx.fillRect(x0, rect.top, x1 - x0, rect.bottom - rect.top)
                        const mid = (x0 + x1) / 2
                        const amp = Math.max(1, (x1 - x0) / 4)
                        ctx.strokeStyle = Theme.inkFaint
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(mid, rect.top)
                        let side = -1
                        for (let y = rect.top + 4; y < rect.bottom; y += 4) {
                            ctx.lineTo(mid + side * amp, y)
                            side = -side
                        }
                        ctx.lineTo(mid, rect.bottom)
                        ctx.stroke()
                    }
                }

                // Temperatura externa por baixo da linha da colmeia, cortada na área do gráfico.
                if (m.key === "temp" && root.outsideSegments.length > 0) {
                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(root.padLeft, rect.top - 2, plotRight - root.padLeft, rect.bottom - rect.top + 4)
                    ctx.clip()
                    ctx.strokeStyle = root.outsideColor
                    ctx.lineWidth = 1.5
                    ctx.lineJoin = "round"
                    ctx.lineCap = "butt"
                    ctx.setLineDash([5, 4])
                    ctx.beginPath()
                    for (const segment of root.outsideSegments) {
                        for (let k = 0; k < segment.length; k++) {
                            const x = root.xFor(segment[k].t)
                            const y = root.yFor(segment[k].temp, scale, rect)
                            if (k === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                    }
                    ctx.stroke()
                    ctx.setLineDash([])
                    ctx.restore()
                }

                // A linha: 2 px, cantos arredondados; interrompida onde a placa ficou offline.
                ctx.strokeStyle = m.color
                ctx.lineWidth = 2
                ctx.lineJoin = "round"
                ctx.lineCap = "round"
                ctx.beginPath()
                let prevT = null
                let last = null
                for (const p of root.points) {
                    const v = p[m.key]
                    if (v === undefined || v === null) { prevT = null; continue }
                    const x = root.xFor(p.t)
                    const y = root.yFor(v, scale, rect)
                    if (prevT === null || p.t - prevT > root.gapMs)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                    prevT = p.t
                    last = { x: x, y: y }
                }
                ctx.stroke()

                // Ponto final (raio 4) com um anel de 2 px da cor do fundo.
                if (last) {
                    ctx.beginPath()
                    ctx.arc(last.x, last.y, 6, 0, Math.PI * 2)
                    ctx.fillStyle = root.surfaceColor
                    ctx.fill()
                    ctx.beginPath()
                    ctx.arc(last.x, last.y, 4, 0, Math.PI * 2)
                    ctx.fillStyle = m.color
                    ctx.fill()
                }

                // Ponto da mira nesta faixa.
                if (root.hoverIndex >= 0) {
                    const p = root.points[root.hoverIndex]
                    const v = p[m.key]
                    if (v !== undefined && v !== null) {
                        const x = root.xFor(p.t)
                        const y = root.yFor(v, scale, rect)
                        ctx.beginPath()
                        ctx.arc(x, y, 6, 0, Math.PI * 2)
                        ctx.fillStyle = root.surfaceColor
                        ctx.fill()
                        ctx.beginPath()
                        ctx.arc(x, y, 4, 0, Math.PI * 2)
                        ctx.fillStyle = m.color
                        ctx.fill()
                    }
                }
            }

            // Eixo de tempo compartilhado.
            ctx.fillStyle = root.axisTextColor
            ctx.textAlign = "center"
            ctx.textBaseline = "top"
            // Marcas igualmente espaçadas na tela; juntando os períodos sem dados,
            // a hora de cada uma vem de timeAt (e some se cair numa faixa).
            const tickCount = Math.max(2, Math.min(6, Math.floor((plotRight - root.padLeft) / 90)))
            for (let i = 0; i <= tickCount; i++) {
                const x = root.padLeft + (plotRight - root.padLeft) * i / tickCount
                const t = root.timeAt(x)
                if (t === null)
                    continue
                ctx.textAlign = i === 0 ? "left" : (i === tickCount ? "right" : "center")
                ctx.fillText(root.timeLabel(t, false), x, height - root.padBottom + 8)
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: root.points.length === 0
        text: "Sem leituras neste período"
        color: root.axisTextColor
        font.pixelSize: 14
    }

    // Passar o mouse (computador) ou tocar e arrastar (celular) mostra os valores daquele momento.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: (mouse) => root.hoverIndex = root.indexAt(mouse.x)
        onPressed: (mouse) => root.hoverIndex = root.indexAt(mouse.x)
        onExited: if (!pressed) root.hoverIndex = -1
    }

    // Dica: texto na cor da tinta; cada série é identificada pelo traço colorido.
    Rectangle {
        id: tooltip
        visible: root.hoverIndex >= 0 && root.hoverIndex < root.points.length
        readonly property var point: visible ? root.points[root.hoverIndex] : null
        readonly property real anchorX: point ? root.xFor(point.t) : 0

        x: Math.max(0, Math.min(root.width - width,
                                anchorX + 12 + width > root.width ? anchorX - width - 12 : anchorX + 12))
        y: root.padTop + root.bandTitleHeight   // abaixo do título da primeira faixa
        width: tooltipColumn.implicitWidth + 16
        height: tooltipColumn.implicitHeight + 12
        radius: Theme.radius
        color: Theme.paper
        border.color: Theme.inkFaint

        Column {
            id: tooltipColumn
            x: 8
            y: 6
            spacing: 3

            Text {
                text: tooltip.point ? root.timeLabel(tooltip.point.t, true) : ""
                color: Theme.inkSoft
                font.pixelSize: 12
            }

            Repeater {
                model: root.bands

                Row {
                    required property string modelData
                    readonly property var m: root.measure(modelData)
                    readonly property var value: tooltip.point ? tooltip.point[modelData] : null
                    spacing: 6

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 2
                        radius: 1
                        color: parent.m.color
                    }
                    Text {
                        text: parent.m.title + ": "
                              + (parent.value === null || parent.value === undefined
                                 ? "-" : root.number(parent.value, parent.m.decimals) + " " + parent.m.unit)
                        color: Theme.ink
                        font.pixelSize: Theme.small
                    }
                }
            }

            // Temperatura externa no mesmo instante (traço tracejado, como no gráfico).
            Row {
                readonly property var value: tooltip.point ? root.outsideAt(tooltip.point.t) : null
                visible: root.outsideSegments.length > 0
                spacing: 6

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Repeater {
                        model: 3
                        Rectangle { width: 3; height: 2; color: root.outsideColor }
                    }
                }
                Text {
                    text: "Externa: " + (parent.value === null ? "-" : root.number(parent.value, 1) + " °C")
                    color: Theme.ink
                    font.pixelSize: Theme.small
                }
            }
        }
    }
}
