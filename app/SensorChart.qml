import QtQuick

// Gráfico no tempo da temperatura e/ou umidade de uma placa.
//
// "both" empilha uma faixa de temperatura sobre uma de umidade, com o mesmo
// eixo de tempo e uma só mira, para comparar momento a momento enquanto cada
// uma mantém a própria escala (duas escalas no mesmo gráfico tornariam os
// cruzamentos e as alturas relativas das linhas sem sentido).
Item {
    id: root

    property var points: []          // [{t: ms desde a época, temp, hum}], do mais antigo ao mais novo
    property real bucketMs: 60000    // largura de um ponto (ver gapMs)
    property real startMs: 0         // faixa do eixo x
    property real endMs: 0
    property string mode: "both"     // "both" | "temp" | "hum"

    readonly property color tempColor: Theme.propolis
    readonly property color humColor: Theme.sky
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

    readonly property var bands: mode === "both" ? ["temp", "hum"] : [mode]

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
        const span = Math.max(1, endMs - startMs)
        return padLeft + (t - startMs) / span * (width - padLeft - padRight)
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
        return minV === Infinity ? null : niceScale(minV, maxV)
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
        return Qt.formatDateTime(d, spanH <= 36 ? "HH:mm" : "dd/MM")
    }

    // Números em pt-BR: vírgula decimal.
    function number(value, decimals) {
        return Number(value).toLocaleString(Qt.locale(), "f", decimals)
    }

    // Ponto mais próximo de x (os pontos estão em ordem de tempo).
    function indexAt(x) {
        if (points.length === 0) return -1
        const t = startMs + (x - padLeft) / (width - padLeft - padRight) * (endMs - startMs)
        let lo = 0, hi = points.length - 1
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            if (points[mid].t < t) lo = mid + 1; else hi = mid
        }
        if (lo > 0 && Math.abs(points[lo - 1].t - t) < Math.abs(points[lo].t - t))
            lo -= 1
        return lo
    }

    onPointsChanged: { hoverIndex = -1; canvas.requestPaint() }
    onModeChanged: canvas.requestPaint()
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
                ctx.fillText(m.title + " (" + m.unit + ")", root.padLeft + 20, rect.top - root.bandTitleHeight + 9)
                ctx.font = root.canvasFont

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
            const tickCount = Math.max(2, Math.min(6, Math.floor((plotRight - root.padLeft) / 90)))
            for (let i = 0; i <= tickCount; i++) {
                const t = root.startMs + (root.endMs - root.startMs) * i / tickCount
                const x = root.xFor(t)
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
        }
    }
}
