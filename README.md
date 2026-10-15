# ProjExtensão UNEB Abelhas

Monitoramento de colmeias com ESP32: uma placa dentro da colmeia mede
temperatura e umidade, e o app **Abelhas Petterson** (Windows e Android)
mostra as leituras ao vivo, o histórico e as estatísticas de cada colmeia.
Projeto de extensão da UNEB.

```
 Colmeia                         Nuvem                     Celular / PC
┌───────────────┐   Wi-Fi    ┌──────────────┐  internet  ┌──────────────────┐
│ ESP32 + DHT22 │ ─────────► │   Supabase   │ ─────────► │ App Abelhas      │
│ + cartão SD   │            └──────────────┘            │ Petterson        │
│               │ ──────────── Bluetooth (ao vivo) ────► │                  │
└───────────────┘                                        └──────────────────┘
```

## Funcionalidades

**App**

- Lista das colmeias com temperatura e umidade em tempo real.
- Faixa das últimas 24 h de cada colmeia: mostra se ela está mais quente ou
  mais fria que no resto do dia.
- Aviso em vermelho quando uma placa fica mais de uma hora sem enviar leituras.
- Estatísticas por colmeia (24 h, 7 dias, 30 dias ou tudo): média, mínima e
  máxima com horário, amplitude, desvio padrão, variação e correlação entre
  temperatura e umidade.
- Gráfico de temperatura e umidade no tempo, com mira para ver os valores de
  cada momento. Opcionalmente, junta os períodos em que a placa ficou
  desligada, para as horas medidas ocuparem o gráfico todo.
- Temperatura externa da cidade do apiário (Open-Meteo, gratuito), tracejada
  no gráfico e nas estatísticas: média lá fora e quanto a colmeia fica acima
  dela. A cidade é escolhida em **Local do apiário**, no menu.
- Exportar as leituras (por colmeia e por período) para uma planilha CSV,
  que abre direto no Excel, LibreOffice ou Google Planilhas.
- Funciona sem internet: guarda as leituras no aparelho e envia para a nuvem
  quando a conexão volta.
- Ferramentas da placa por Bluetooth: configurar o Wi-Fi (com lista das redes
  que a placa enxerga), renomear, mudar o intervalo de registro e reiniciar.
- Limpar dados deste aparelho e/ou da nuvem, por colmeia e por período (apagar
  na nuvem exige a senha de administrador).

**Placa (ESP32)**

- Lê o sensor a cada 2 s e envia o valor ao vivo por Bluetooth.
- A cada intervalo de registro (1 a 60 min, padrão 1 min) grava a média como
  um registro e envia ao Supabase.
- Sem internet, guarda os registros no cartão SD e envia depois.
- Hora certa pela internet ou, sem internet, pelo celular conectado.
- Abre o hotspot **Colmeia-Setup** para configurar o Wi-Fi quando não tem rede.

## Estrutura do repositório

| Pasta | Conteúdo |
|---|---|
| `app/` | App em Qt 6 / QML (Windows e Android) |
| `firmware/ColmeiaMonitor/` | Código da ESP32 (Arduino) |
| `supabase/` | Estrutura do banco de dados na nuvem |

## Instalação do app

Baixe a versão mais recente na página de
[Releases](https://github.com/gabrielBispoTolentino/ProjExtensao-UNEB-Abelhas/releases):

- **Windows:** baixe o `.zip`, extraia e abra `AbelhasPetterson.exe`. Não
  precisa instalar.
- **Android (9 ou mais novo):** baixe o `.apk` no celular e abra. O Android
  vai pedir para permitir a instalação de fontes desconhecidas. Na primeira
  abertura, permita o Bluetooth (e a localização, no Android 11 ou anterior).

O app já vem ligado ao banco do projeto: as colmeias aparecem assim que as
placas enviam leituras.

## Configuração

### 1. Banco de dados (Supabase)

Só é preciso para criar um banco novo; o repositório já usa o do projeto.

1. Crie um projeto em [supabase.com](https://supabase.com).
2. No painel, abra **SQL Editor → New query**, cole o conteúdo de
   `supabase/schema.sql` e clique em **Run**.
3. Defina a senha de administrador (usada para apagar dados pelo app), no
   mesmo SQL Editor:
   ```sql
   insert into private.admin_password (password_hash)
   values (extensions.crypt('SUA-SENHA', extensions.gen_salt('bf', 10)))
   on conflict (id) do update set password_hash = excluded.password_hash;
   ```
4. Em **Project Settings → API**, copie a URL do projeto e a chave pública
   (*publishable* / *anon*, nunca a *service_role*) para
   `app/supabaseconfig.h` e para `SUPABASE_URL` / `SUPABASE_KEY` no
   firmware.

### 2. Montagem da placa

| Peça | Ligação na ESP32 |
|---|---|
| DHT22 (dados) | GPIO 32 |
| Módulo microSD | CS 5, SCK 18, MISO 19, MOSI 23 |
| Alimentação | VCC e GND de cada módulo (veja a tensão do seu módulo SD) |

O sensor fica na melgueira, logo acima do ninho, exposto ao ar que sobe por
uma tela.

### 3. Gravação do firmware

1. Instale a [Arduino IDE](https://www.arduino.cc/en/software) e, no
   gerenciador de placas, **esp32 by Espressif** (versão 3.3.x).
2. No gerenciador de bibliotecas, instale **DHT sensor library** (Adafruit),
   **ArduinoJson** (7.x) e **NimBLE-Arduino** (2.x).
3. Abra `firmware/ColmeiaMonitor/ColmeiaMonitor.ino`.
4. Em **Ferramentas**, escolha a placa **ESP32 Dev Module** e o
   **Partition Scheme: Huge APP**.
5. Conecte a placa pelo USB e clique em **Carregar**.

### 4. Primeiro uso

1. Ligue a placa e abra o app perto dela, com o Bluetooth ligado.
2. No menu do app, use **Configurar Wi-Fi**, escolha a rede e digite a senha.
3. Sem o app, também dá para conectar o celular ao Wi-Fi **Colmeia-Setup**:
   a página de configuração abre sozinha.

## Compilar o app

Requisitos: **Qt 6.11** com os kits *MinGW 64-bit* (Windows) e/ou *Android*,
e o módulo **Qt Connectivity** (Bluetooth), instalados pelo Qt Maintenance
Tool. Para Android, também o Android SDK, o NDK 27 e o JDK 17.

Abra `app/CMakeLists.txt` no Qt Creator, escolha o kit e compile. Pela linha
de comando (Windows):

```sh
cmake -S app -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64
cmake --build build
```

O banco local do app fica em
`%APPDATA%\AbelhasPetterson\SensorMonitor\sensor_readings.db` no Windows.

## Organização do código

- **Firmware:** um único arquivo, dividido em seções (configuração, Wi-Fi,
  cartão SD, Supabase, Bluetooth, `setup()` e `loop()`).
- **App:** a parte em C++ cuida dos dados (`sensormonitor`, `cloudsync`,
  `sensorreadingstore`, `outsideweather`, `blesensorclient`,
  `boardcontrol`); as telas ficam nos arquivos `.qml`, com cores e tamanhos
  centralizados em `Theme.qml`.
- **Comentários** em português: cada arquivo começa explicando o que faz,
  arquivos longos são divididos em seções (`// ── Nome ──`) e os comentários
  explicam o porquê, não o óbvio. Nomes de funções e variáveis ficam em inglês.

A fonte Atkinson Hyperlegible, usada no app, é distribuída sob a SIL Open
Font License (`app/fonts/OFL.txt`).
