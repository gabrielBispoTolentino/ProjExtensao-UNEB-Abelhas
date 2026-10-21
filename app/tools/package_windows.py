"""Monta o .zip do Windows de uma versão do BeeDuino.

    python tools/package_windows.py <appBeeDuino.exe do build Release> <versão> <pasta de saída>

Gera <saída>/BeeDuino-<versão>-windows/ e BeeDuino-<versão>-windows.zip:

  * BeeDuino.exe e uma cópia AbelhasPetterson.exe: o atualizador das versões
    1.2.0 a 1.4.0 (nome antigo) procura esse arquivo no .zip; o script de
    instalação novo não a instala (ver updater/install-windows.ps1);
  * windeployqt sem o que o app não usa: OpenGL por software (o Qt Quick no
    Windows desenha com Direct3D 11), D3DCompiler (já vem no Windows 10/11),
    traduções do Qt, plugins de imagem/TLS/toque/rede que nada carrega;
  * só o estilo Basic dos controles: o windeployqt copia todos os estilos
    porque o seletor de arquivos do Qt tem uma versão para cada um, mas o
    app fixa o Basic (main.cpp), então os outros nunca são carregados.

Precisa do Qt no PATH (windeployqt) ou de QT_BIN apontando para a pasta bin.
O .zip usa caminhos \\\\?\\ porque alguns arquivos do Qt passam de 260
caracteres em pastas fundas.
"""

import os
import shutil
import subprocess
import sys
import zipfile

exe, version, out = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
qt_bin = os.environ.get('QT_BIN', '')
windeployqt = os.path.join(qt_bin, 'windeployqt.exe') if qt_bin else 'windeployqt'
app_qml = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

name = 'BeeDuino-%s-windows' % version
folder = os.path.join(out, name)
shutil.rmtree(folder, ignore_errors=True)
os.makedirs(folder)
shutil.copy2(exe, os.path.join(folder, 'BeeDuino.exe'))

subprocess.run([windeployqt, '--release', '--no-translations', '--no-opengl-sw', '--no-system-d3d-compiler',
                '--exclude-plugins',
                'qgif,qjpeg,qico,qcertonlybackend,qtuiotouchplugin,qnetworklistmanager,'
                'qsqlodbc,qsqlpsql,qsqlmimer,qsqlibase,qsqloci',
                '--skip-plugin-types', 'qmltooling',
                '--qmldir', app_qml, os.path.join(folder, 'BeeDuino.exe')], check=True)

# ── Só o estilo Basic ───────────────────────────────────────────────────────
unused_styles = ['FluentWinUI3', 'Fusion', 'Imagine', 'Material', 'Universal', 'Windows', 'iOS', 'macOS']
long = lambda p: '\\\\?\\' + os.path.abspath(p)
for style in unused_styles:
    shutil.rmtree(long(os.path.join(folder, 'qml', 'QtQuick', 'Controls', style)), ignore_errors=True)
    shutil.rmtree(long(os.path.join(folder, 'qml', 'QtQuick', 'Dialogs', 'quickimpl', 'qml', '+' + style)),
                  ignore_errors=True)
shutil.rmtree(long(os.path.join(folder, 'qml', 'QtQuick', 'NativeStyle')), ignore_errors=True)
for dll in os.listdir(folder):
    if dll.startswith('Qt6QuickControls2') and any(s in dll for s in unused_styles):
        os.remove(os.path.join(folder, dll))

# O atualizador antigo procura AbelhasPetterson.exe (ver o comentário no topo).
shutil.copy2(os.path.join(folder, 'BeeDuino.exe'), os.path.join(folder, 'AbelhasPetterson.exe'))

# ── .zip com a pasta BeeDuino-<versão>-windows/ dentro ──────────────────────
archive = os.path.join(out, name + '.zip')
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(long(folder)):
        for f in sorted(files):
            path = os.path.join(root, f)
            z.write(path, os.path.relpath(path, long(out)).replace(os.sep, '/'))

total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(long(folder)) for f in fs)
count = sum(len(fs) for _, _, fs in os.walk(long(folder)))
print('%s: %d arquivos, %.1f MB instalado, .zip %.1f MB'
      % (name, count, total / 1e6, os.path.getsize(archive) / 1e6))
