#pragma once

#include <QString>

// Até a versão 1.4.0 o app se chamava Abelhas Petterson e, no Windows,
// guardava o banco local em %APPDATA%\AbelhasPetterson\SensorMonitor; desde
// a BeeDuino, em %APPDATA%\BeeDuino\SensorMonitor. No Android a pasta não
// muda (é a pasta privada do app, que não depende do nome).

// Pasta de dados a usar. Na primeira abertura depois da troca de nome, move
// a pasta antiga para o lugar novo. Se a pasta antiga tem dados mas não pôde
// ser movida (ex.: a versão antiga ainda está aberta com o banco), continua
// usando a antiga nesta abertura, para os dados não ficarem divididos.
// Chamar depois de definir o nome da organização e do app.
QString resolveDataDir();

// O passo testável: move oldDir para newDir se só a antiga tem o banco.
// Retorna a pasta a usar; *log recebe o que aconteceu (vazio se nada).
QString moveLegacyDataDir(const QString &oldDir, const QString &newDir, QString *log);
