"""Filtro da saída do qmlimportscanner no build do Android: tira os estilos
dos controles que o app nunca usa, para eles não irem no APK.

O seletor de arquivos do Qt (QtQuick.Dialogs) tem uma versão para cada estilo
(Fusion, Material...), e o scanner segue todas; o app fixa o estilo Basic
(main.cpp e os imports QtQuick.Controls.Basic), então os outros nunca são
carregados. O CMakeLists.txt põe este filtro entre o androiddeployqt e o
qmlimportscanner de verdade (lê o JSON do scanner na entrada padrão).
"""

import json
import re
import sys

UNUSED = re.compile(r'^QtQuick\.(Controls\.(FluentWinUI3|Fusion|Imagine|Material|Universal|Windows|iOS|macOS)'
                    r'|NativeStyle)(\.|$)'
                    # O QtQuick.Controls genérico também sai: o qmldir dele importa todos
                    # os estilos (e o androiddeployqt leva a pasta inteira). O app só usa
                    # QtQuick.Controls.Basic e .impl, que continuam.
                    r'|^QtQuick\.Controls$')

text = sys.stdin.read()
try:
    imports = json.loads(text)
except ValueError:
    sys.stdout.write(text)   # não é o JSON esperado: repassa sem mexer
    sys.exit(0)

kept = [i for i in imports if not UNUSED.match(i.get('name', ''))]
sys.stdout.write(json.dumps(kept, indent=2))
sys.stderr.write('qml_imports_basic_only: %d of %d imports kept\n' % (len(kept), len(imports)))
