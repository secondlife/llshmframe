@if not exist "tools/" (
    @echo Run this command from the project root directory
    @goto end
)

if exist build rmdir build /s /q

cmake -B build
cmake --build build --config Release

:end
