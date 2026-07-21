# VPN (SOCKS5-прокси)

Небольшой асинхронный SOCKS5-прокси-сервер на C++23. Сервер принимает TCP-подключения, соединяется с указанным клиентом узлом и передаёт трафик в обе стороны.

Сейчас поддерживаются:

- команда SOCKS5 `CONNECT`;
- IPv4, IPv6 и доменные имена;
- подключение без авторизации;
- раздельные журналы сервера и пользовательских сессий.

Команды `BIND`, `UDP ASSOCIATE` и авторизация по логину и паролю пока не поддерживаются.

## Требования

- CMake 3.20 или новее;
- компилятор с поддержкой C++23 (GCC 13+, Clang 16+ или актуальный Apple Clang);
- OpenSSL;
- Git и доступ в интернет при первой сборке.

Boost 1.90.0, fmt 11.2.0, spdlog 1.15.3 и GoogleTest 1.17.0 загружаются автоматически через CMake `FetchContent`.

### macOS

Установите Xcode Command Line Tools, CMake, Ninja и OpenSSL:

```bash
xcode-select --install
brew install cmake ninja openssl@3
```

Проект по умолчанию собирается для Apple Silicon (`arm64`).

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git libssl-dev
```

Проверьте, что установленный компилятор поддерживает C++23. В старых выпусках Ubuntu может потребоваться более новая версия GCC или Clang.

## Конфигурация

Приложение читает настройки из каталога `config` относительно текущей рабочей директории. Поэтому готовый бинарник следует запускать из корня репозитория.

### Сервер

Файл `config/server.json`:

```json
{
  "server": {
    "ip": "127.0.0.1",
    "port": 1080
  }
}
```

- `ip` — IPv4- или IPv6-адрес, на котором сервер принимает подключения;
- `port` — порт от `0` до `65535`.

Адрес `127.0.0.1` разрешает подключения только с локального компьютера. Чтобы принимать подключения со всех сетевых интерфейсов, укажите `0.0.0.0`. Не публикуйте прокси в интернет без авторизации, сетевого экрана и ограничения доступа по IP.

### Логирование

Файл `config/loggers.json`:

```json
{
  "log_directory": "./logs",
  "loggers": [
    {
      "module_name": "session",
      "filename": "session.log",
      "pattern": "[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] %v",
      "level": "debug",
      "rotation_hour": 12,
      "rotation_minute": 5
    },
    {
      "module_name": "server",
      "filename": "server.log",
      "pattern": "[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] %v",
      "level": "debug",
      "rotation_hour": 12,
      "rotation_minute": 5
    }
  ]
}
```

- `log_directory` — существующий каталог для журналов;
- `module_name` — уникальное имя модуля (`server` и `session` нужны приложению);
- `filename` — уникальное имя файла без пути;
- `pattern` — шаблон spdlog, содержащий `%v` или `%+`;
- `level` — один из уровней: `trace`, `debug`, `info`, `warning`, `error`;
- `rotation_hour` — час ежедневной ротации, от `0` до `23`;
- `rotation_minute` — минута ротации, от `0` до `59`.

Каталог из `log_directory` должен существовать до запуска:

```bash
mkdir -p logs
```

## Сборка

Из корня репозитория выполните:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Если Ninja не установлен, уберите параметр `-G Ninja`.

Для сборки без тестов добавьте `-DVPN_BUILD_TESTS=OFF` при конфигурации CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVPN_BUILD_TESTS=OFF
cmake --build build --parallel
```

## Запуск

Запускайте сервер из корня репозитория, чтобы он нашёл `./config`:

```bash
./build/vpn
```

При стандартной конфигурации SOCKS5-прокси будет доступен по адресу `127.0.0.1:1080`. Для остановки нажмите `Ctrl+C`.

## Проверка

В другом терминале выполните HTTP-запрос через прокси:

```bash
curl --proxy socks5h://127.0.0.1:1080 https://example.com
```

Префикс `socks5h` передаёт разрешение доменного имени прокси-серверу. Для проверки внешнего IP можно использовать:

```bash
curl --proxy socks5h://127.0.0.1:1080 https://api.ipify.org
```

## Тесты

Тесты собираются по умолчанию. После сборки запустите:

```bash
ctest --test-dir build --output-on-failure
```

## Частые ошибки

- `Failed to load config files` — запускайте бинарник из корня репозитория и проверьте наличие `config/server.json` и `config/loggers.json`.
- Ошибка про `log_directory` — создайте указанный каталог и проверьте путь в `config/loggers.json`.
- `Address already in use` — выбранный порт занят; измените `server.port` или остановите использующий его процесс.
- Ошибка OpenSSL при конфигурации — установите пакет OpenSSL для разработки (`libssl-dev` в Ubuntu/Debian или `openssl@3` в Homebrew).
- Ошибка загрузки зависимостей — первая конфигурация CMake требует доступа к GitHub.
