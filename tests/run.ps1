# Run from any directory; requires native Windows GCC (not the RISC-V compiler).
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Path 'build/native' -Force | Out-Null
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/test_link.c src/link.c -o build/native/test_link.exe
    if ($LASTEXITCODE) { throw 'Compile link tests failed' }
    & ./build/native/test_link.exe
    if ($LASTEXITCODE) { throw 'Link tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/test_dap.c src/dap.c -o build/native/test_dap.exe
    if ($LASTEXITCODE) { throw 'Compile DAP tests failed' }
    & ./build/native/test_dap.exe
    if ($LASTEXITCODE) { throw 'DAP tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I tests/host_mock -I include tests/test_host.c src/link.c -o build/native/test_host.exe
    if ($LASTEXITCODE) { throw 'Compile host tests failed' }
    & ./build/native/test_host.exe
    if ($LASTEXITCODE) { throw 'Host tests failed' }
    # Target USB DMA address casts intentionally truncate pointers to hardware
    # register width; register-model tests do not dereference those DMA values.
    gcc -std=gnu99 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -I tests/usb_mock -I include tests/test_usb.c -o build/native/test_usb.exe
    if ($LASTEXITCODE) { throw 'Compile USB tests failed' }
    & ./build/native/test_usb.exe
    if ($LASTEXITCODE) { throw 'USB tests failed' }
    gcc -std=gnu99 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -DWD_USB_POLLING=1 -I tests/usb_mock -I include tests/test_usb.c -o build/native/test_usb_poll.exe
    if ($LASTEXITCODE) { throw 'Compile USB polling tests failed' }
    & ./build/native/test_usb_poll.exe
    if ($LASTEXITCODE) { throw 'USB polling tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I tests/uart_mock -I include tests/test_uart_log.c -o build/native/test_uart_log.exe
    if ($LASTEXITCODE) { throw 'Compile UART log tests failed' }
    & ./build/native/test_uart_log.exe
    if ($LASTEXITCODE) { throw 'UART log tests failed' }
    # C++ register proxies observe the real swd.c GPIO edges and PIN reads.
    g++ -std=c++11 -Wall -Wextra -Werror -I tests/swd_mock -I include tests/test_swd.cpp -o build/native/test_swd.exe
    if ($LASTEXITCODE) { throw 'Compile SWD edge tests failed' }
    & ./build/native/test_swd.exe
    if ($LASTEXITCODE) { throw 'SWD edge tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/test_retry.c src/retry.c -o build/native/test_retry.exe
    if ($LASTEXITCODE) { throw 'Compile adaptive retry tests failed' }
    & ./build/native/test_retry.exe
    if ($LASTEXITCODE) { throw 'Adaptive retry tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/test_rf_config.c src/rf_config.c -o build/native/test_rf_config.exe
    if ($LASTEXITCODE) { throw 'Compile RF config tests failed' }
    & ./build/native/test_rf_config.exe
    if ($LASTEXITCODE) { throw 'RF config tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -DPLATFORM_TEST=1 -I tests/platform_mock -I include tests/test_platform.c src/platform.c -o build/native/test_platform.exe
    if ($LASTEXITCODE) { throw 'Compile platform tests failed' }
    & ./build/native/test_platform.exe
    if ($LASTEXITCODE) { throw 'Platform tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -I tests/bridge_mock -I include tests/test_uart_bridge.c -o build/native/test_uart_bridge.exe
    if ($LASTEXITCODE) { throw 'Compile UART bridge tests failed' }
    & ./build/native/test_uart_bridge.exe
    if ($LASTEXITCODE) { throw 'UART bridge tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -DWD_UART_LOG=0 -I tests/bridge_mock -I include tests/test_uart_bridge.c -o build/native/test_uart_bridge_nolog.exe
    if ($LASTEXITCODE) { throw 'Compile no-log UART bridge tests failed' }
    & ./build/native/test_uart_bridge_nolog.exe
    if ($LASTEXITCODE) { throw 'No-log UART bridge tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -I tests/radio_mock -I include tests/test_radio.c -o build/native/test_radio.exe
    if ($LASTEXITCODE) { throw 'Compile radio recovery tests failed' }
    & ./build/native/test_radio.exe
    if ($LASTEXITCODE) { throw 'Radio recovery tests failed' }
    gcc -std=c99 -Wall -Wextra -Werror -I tests/app_mock -I include tests/test_app.c src/link.c src/retry.c -o build/native/test_app.exe
    if ($LASTEXITCODE) { throw 'Compile unified app tests failed' }
    & ./build/native/test_app.exe
    if ($LASTEXITCODE) { throw 'Unified app tests failed' }
    Write-Output 'All native suites passed, including composite USB, UART IRQ bridge, adaptive retry, persistence, platform and SWD.'
} finally {
    Pop-Location
}
