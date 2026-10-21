# AoSCAD Studio

Самостоятельная настольная CAD-среда. Основная реализация — C++17, Qt 6 и Open CASCADE; это отдельное приложение, а не плагин для другой CAD-системы.

## Что реализовано в нативной C++ версии

- 3D-вид с орбитой, панорамированием, масштабом и стандартными видами; выбор объектов мышью.
- Примитивы: бокс, цилиндр, сфера, конус и тор; перемещение, вращение и масштабирование мышью или через свойства.
- Дерево компонентов, скрытие объектов, цвет, единицы мм/см/дюйм, отмена и повтор.
- Импорт STEP/STP, IGES/IGS, BREP, STL и OBJ через Open CASCADE.
- Экспорт STEP, IGES, BREP, STL и OBJ; проект `.aoscad` сохраняет BREP-геометрию и свойства объектов.
- Инструмент измерения расстояния. Для Aeros Engine предусмотрен экспорт STL.

## Сборка C++ приложения

Нужны CMake 3.22+, C++17, Qt 6.5+ (Core, Gui, Widgets, OpenGL, OpenGLWidgets) и Open CASCADE с модулями обмена STEP/IGES и STL.

### Windows с vcpkg

В репозитории есть `vcpkg.json` с зависимостями Qt и Open CASCADE. В PowerShell, из каталога репозитория:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Запускаемый файл появится в `build/Release/aoscad-studio.exe` (или `build/aoscad-studio.exe`, в зависимости от генератора CMake).

### Windows Release installer

Публикация GitHub Release `v0.2.0` запускает workflow `Windows C++ installer`: он собирает приложение, разворачивает Qt/Open CASCADE runtime, создаёт per-user установщик `AoSCAD-Studio-Setup-0.2.0-x64.exe` и прикладывает его к релизу. Установщик пока не подписан сертификатом издателя, поэтому Windows SmartScreen может показать предупреждение.

### Linux / macOS или ручная установка библиотек

Установите Qt 6 и Open CASCADE development packages, затем укажите CMake пути к ним при необходимости:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/path/to/Qt;/path/to/OpenCASCADE"
cmake --build build --config Release
```

CMake target: `aoscad-studio`.

## Совместимость SolidWorks, T-FLEX и других CAD-систем

Нативные файлы SolidWorks (`.SLDPRT`, `.SLDASM`) и T-FLEX (`.TFC`, `.GRB`) напрямую не читаются: их форматы проприетарны. Экспортируйте модель из исходной программы в **STEP AP214/AP242** или **IGES**, затем откройте её здесь. Эти форматы предназначены для обмена между CAD-системами; при экспорте в STL/OBJ передаётся только полигональная сетка, без параметрической истории.

Для Aeros Engine экспортируйте STL и импортируйте его там. AoSCAD не заменяет расчёт аэродинамики в Aeros Engine.

## Примечание о переходе

Каталоги `src/`, `electron/` и файлы npm оставлены как прежняя браузерная/Electron реализация. Нативная C++ сборка запускается через CMake; Windows workflow собирает её и создаёт Inno Setup installer. Старый релиз `v0.1.0` относится к Electron-версии. Старые `.aoscad`-проекты из Web/Electron-версии пока не переносятся напрямую: откройте их в прежней версии и экспортируйте геометрию в STL или OBJ.
