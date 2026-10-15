pragma Singleton

import QtQuick

// Valores do visual do app (cores, tamanhos de texto). As cores vêm do ícone
// da abelha (icons/generate_icons.py): contorno de tinta, corpo de mel, asa
// clara. As cores de texto têm contraste de pelo menos 4,5:1 sobre paper e
// wing; inkFaint (3,3:1+) só serve para bordas e texto desabilitado.
QtObject {
    readonly property color ink: "#2B2118"         // texto, contornos
    readonly property color inkSoft: "#5E5246"     // texto secundário
    readonly property color inkFaint: "#8F857B"    // bordas de controles, desabilitado
    readonly property color rule: "#DDD6CD"        // divisórias

    readonly property color honey: "#FFC83A"       // faixa do topo, ação principal, seleção
    readonly property color honeyDeep: "#F0B21C"   // mel pressionado
    readonly property color honeyPale: "#FFF2CC"   // linhas selecionadas, avisos

    readonly property color wing: "#EEF5FC"        // fundo das telas
    readonly property color paper: "#FFFFFF"       // folhas, campos

    readonly property color propolis: "#9A4A17"    // temperatura
    readonly property color sky: "#2F6FA8"         // umidade
    readonly property color outside: "#6B6F76"     // temperatura externa (referência, tracejada)
    readonly property color sting: "#B42318"       // erros, apagar
    readonly property color stingPale: "#FBE6E3"

    readonly property string fontFamily: "Atkinson Hyperlegible"

    // Escala de texto (px).
    readonly property int small: 13
    readonly property int body: 15
    readonly property int lead: 18
    readonly property int title: 24
    readonly property int figure: 32               // valores na tela de estatísticas
    readonly property int reading: 48              // leituras ao vivo na lista de colmeias

    readonly property int touch: 48                // altura mínima de itens tocáveis
    readonly property int radius: 8
}
