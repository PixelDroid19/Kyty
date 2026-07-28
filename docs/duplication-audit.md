# Auditoría de duplicación lógica

Estado: correcciones implementadas y verificadas en el árbol de trabajo del
27 de julio de 2026.

La regla aplicada fue **un comportamiento, una implementación**. Cada
extracción elimina el cuerpo anterior en el mismo cambio: no se conservaron
wrappers de compatibilidad, aliases de transición ni rutas permisivas para
ocultar entradas no soportadas.

## Centralizaciones terminadas

| Área | Implementación canónica | Copias eliminadas o consumidores migrados |
| --- | --- | --- |
| Registros PM4 de color/profundidad/stencil | `GraphicsState::DecodeColorInfo`, `DecodeDepthZInfo` y `DecodeDepthStencilInfo` | Caminos directo, batched e indirecto de `GraphicsRun.cpp`. Stencil consume el único dword válido (`buffer[0]`). |
| Resolución HLE | Clave exacta `(NID, library, module, versions, type)` en `SymbolDatabase::Find` | Se eliminó su fallback global solo por NID, los binds catch-all de AudioOut2 y los bloques de inicialización repetidos. `libkernel_unity` se registra como identidad explícita; `PSNCommon`, `NpAuth` y `NpUtility` sin ABI verificado no devuelven éxito genérico. `KernelDlsym` conserva una búsqueda explícita limitada al módulo seleccionado. |
| Registro de librerías | `LibraryRegistration.h` y una lista de exports por contrato | SaveData y diálogos registran variantes exactas sin copiar bloques ni crear aliases entre identidades. |
| Overlap de memoria GPU | `GpuMemoryOverlap.h` | La ruta lenta duplicada desapareció; clasificación, inversión y políticas consumen una sola semántica. |
| Formatos de imágenes guest | `VulkanImageFormat.h/.cpp` con `GuestImageUsage` | Texture y StorageTexture ya no mantienen tablas ni validadores paralelos. Un uso no soportado devuelve `VK_FORMAT_UNDEFINED` y el caller lo rechaza. |
| Formatos de vertex input | `VulkanVertexInputFormat.h/.cpp` | Shader y GraphicsRender consumen juntos `VkFormat` y cantidad de componentes. Desapareció el tamaño 4 usado para continuar con formatos desconocidos. |
| Creación de imágenes/vistas Vulkan | `VulkanImageBuilder.h/.cpp` | Texture, StorageTexture, RenderTexture, DepthStencil, VideoOut, Window y GraphicsRender. No queda un `VkImageCreateInfo` o `VkImageViewCreateInfo` construido fuera del builder. |
| Fábricas de objetos de imagen | Helpers internos compartidos en Texture y RenderTexture | `Create` y `CreateFromObjects` conservan entradas distintas, pero comparten validación, descriptor y construcción. |
| Geometría tile 64 KiB | `TileGet64KBBlockWidth` y `TileAlign64KBPitch` | Render targets y texturas ya no copian tablas de ancho de bloque. Formatos desconocidos fallan explícitamente. |
| BMP RGBA8 | `UtilWriteRgba8Bmp` | Dumps Vulkan y diagnóstico lineal de Texture usan un único encoder. |
| Escritura atómica host | `AtomicFileWrite` | Pipeline cache, caché SPIR-V y SaveDataMemory comparten temporal único y reemplazo atómico. |
| Runners Python | `scripts/kyty_runner_common.py` | Matrix, playable y capture comparten entorno estricto, socket, identidad del agente, proceso-grupo y terminación. |
| Escape JSON del agente | `Kyty/Agent/Json.h` | CLI y protocolo comparten el mismo encoder; no queda un `JsonEscape` local. |
| Inicialización de memoria virtual | `CoreSubsystem` | Kernel Memory declara la dependencia y ya no reinicializa el estado global. |
| SaveData portable | `SaveDataPaths` y `SaveDataMemoryStore` | Un root por título y un archivo por `(usuario, slot)`; Setup/Get/Set/Sync usan el mismo backend y límites exactos. |
| Ventana e input host | `HostWindowControls` | F11, Alt+Enter, doble clic, pausa, foco y supresión de aristas se deciden en una política testeable. |
| PCM de AudioOut | `AudioPcm` | S16/F32 comparten volumen por canal y cálculo temporal de cola; SDL realiza una única conversión al dispositivo real. |

## Contratos estrictos resultantes

- Una importación desconocida no se resuelve por coincidencia de NID en otra
  librería.
- `Media/Plugins` no forma parte del bootstrap por descubrimiento: un nombre
  de PRX no autoriza su carga ni la invención de argumentos de inicio.
- Las identidades `Posix`, `libkernel` y `libkernel_unity` se prueban de forma
  independiente; una coincidencia de NID no autoriza a cruzar de una a otra.
- La cuenta NP offline informa `SIGNED_OUT`; no se fabrica un identificador de
  cuenta para adaptar el arranque de un título.
- Una lectura o escritura SaveDataMemory antes de `Setup` devuelve
  `SAVE_DATA_ERROR_MEMORY_NOT_READY`.
- SaveDataMemory no crece durante `Get` o `Set`, no trunca rangos y valida un
  lote completo antes de modificarlo.
- `Sync` publica el archivo completo antes de encolar el evento de finalización.
- `KYTY_SAVEDATA_DIR` solo acepta una ruta absoluta. No hay migración desde
  `_SaveData`, redirección a una ruta relativa ni directorio alternativo.
- Un formato Vulkan no soportado se rechaza; no se sustituye por RGBA8.
- Los runners no heredan variables de bring-up permisivo ni matan un PID sin
  comprobar antes la identidad del proceso/agente.

## Contratos de runtime host

La arquitectura SDL2 de Kyty incorpora ventana redimensionable, fullscreen de
escritorio, hotkeys, cursor, eventos de foco/minimizado, hotplug/remap de
mandos, conversión al dispositivo de audio, cola acotada, pausa host y
directorios de usuario portables.

SaveDataMemory persiste por usuario y slot bajo el root del título mediante el
publicador atómico común; no usa un directorio temporal global.

No se portaron:

- una migración completa a SDL3, porque Kyty mantiene un backend SDL2 funcional
  y el cambio no es necesario para esos contratos;
- ATRAC9/AT9, porque exige un decoder y un contrato ABI verificables, no un
  stub que produzca éxito silencioso;
- precargas de plugins, stubs HLE generales o excepciones por título;
- una caché Vulkan adicional, porque Kyty ya valida identidad de dispositivo,
  límites, presupuesto de sesión y publicación atómica.

## Diferencias intencionales que no son duplicación

- Los dos `EventRing` pertenecen a namespaces y contratos distintos: cola del
  agente y telemetría SPSC. Unificarlos alteraría concurrencia y capacidad.
- Encode y decode PM4 son lados productor/consumidor del protocolo.
- NIDs iguales en identidades exactas distintas pueden compartir una función
  cuando el ABI es realmente el mismo; esto no habilita resolución cruzada.
- NGS2 genera/mixea audio guest; AudioOut temporiza y entrega PCM al host.
- Los asignadores libc, mspace y ApplicationHeap implementan contratos guest
  distintos.

## Verificación reproducible

```bash
cmake -S source -B _build_linux
cmake --build _build_linux --target fc_script -j4

_build_linux/fc_script scripts/run_unit_tests.lua \
  --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*:EmulatorSymbolDatabase.*:EmulatorSaveData.*:EmulatorAudio.*:AgentTools.*'

# Suite completa: 909 aprobadas y 1 omitida de forma intencional en Linux
_build_linux/fc_script scripts/run_unit_tests.lua --gtest_color=no

python3 scripts/test_kyty_runner_common.py
python3 scripts/test_kyty_playable_regression.py
python3 scripts/test_kyty_games_matrix.py
```

La validación funcional de un título sigue siendo independiente de estos
contratos. Un test local verde no autoriza a afirmar compatibilidad o boot real
sin ejecutar el workload correspondiente.
