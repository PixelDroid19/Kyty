# Auditoría de módulos duplicados y plan de centralización

Estado: **solo diagnóstico** (nada se ha centralizado todavía).
Base: `main` @ `cd2a753`. Auditoría de lectura sobre `source/emulator/**`,
`source/lib/**`, `source/agent/**`, `scripts/**` y tests.

Regla que motiva este documento (AGENTS.md, invariante 3): *"One behavior,
one implementation"*. Abajo está cada violación encontrada, dónde vive cada
copia, cuál copia debería ser la canónica, y el plan por fases para
centralizar sin romper el frontier.

---

## 1. Hallazgos — stack de gráficos

### Riesgo ALTO

| # | Responsabilidad duplicada | Copias | Canónico propuesto |
|---|---------------------------|--------|--------------------|
| G1 | Decodificación de registros CX PM4 (directo vs indirecto) | `GraphicsRun.cpp:3965-3986` (tabla directa `g_hw_ctx_func`) vs `:3564-3584` + `:4172-4904` (tabla indirecta `g_hw_ctx_indirect_func`) | Parsers directos `hw_ctx_*`; el indirecto debe delegar (patrón ya usado para scissor/blend en `:4179-4230`) |
| G2 | Parse de `CB_COLOR0_INFO` (×3) | `GraphicsRun.cpp:1888-1930`, `:2237-2255`, `:4281-4304` | Un helper `ApplyColorInfoFromPm4(dword, HW::ColorInfo&)` |
| G3 | Parse de `DB_Z_INFO` (×3) | `GraphicsRun.cpp:1970-1981`, `:2000-2009`, `:4760-4774` | Helper único llamado desde ambos caminos |
| G4 | Parse de `DB_STENCIL_INFO` (×3) — además el camino directo lee `buffer[1]` con `cmd_id 0xC0016900` (posible bug de indexado) | `GraphicsState.cpp:322-335`, `GraphicsRun.cpp:2419-2427`, `:4776-4787` | `GraphicsState::ApplyDepthStencilPlaneRegisters` |
| G5 | Efectos de `WRITE_DATA`/`RELEASE_MEM`: pre-scan del stream vs handlers del CP | `Graphics.cpp:78-178` (`GraphicsPm4PublishFenceProducers`) vs `GraphicsRun.cpp:799-834` (`CommandProcessor::WriteData`) y `:3807+` | Handlers del CP; el pre-scan debe llamar helpers compartidos |
| G6 | Tabla formato→VkFormat para texturas sampled vs storage | `Objects/Texture.cpp:31-90` (`TextureResolveSampledVkFormat`) vs `Objects/StorageTexture.cpp:20-42` (`get_texture_format`) | `TextureResolveSampledVkFormat` (StorageTexture ya se quedó sin los formatos Gen5 14/56/71/133) |

### Riesgo MEDIO

| # | Responsabilidad | Copias | Canónico |
|---|-----------------|--------|----------|
| G7 | Pitch de tile 27 (tabla `k_block_w` 64KB) | `GraphicsRender.cpp:4575-4584` vs `:4990-5000` | Función compartida en `Tile.cpp` |
| G8 | Derivación de pitch de textura: objeto vs bind-time | `Objects/Texture.cpp:221-235` vs `GraphicsRender.cpp:4984-5026` | API descriptor→layout única (`Tile.cpp` + `ShaderGen5ResolveLinearPitch`) |
| G9 | Dump BMP RGBA8 (×4) | `Utils.cpp:459-545`, `Window.cpp:3244-3270`, `Window.cpp:533-653`, `Objects/Texture.cpp:310-348` | `UtilDumpVulkanImageRgba8Bmp` |
| G10 | `RenderTextureFormat`→VkFormat duplicado en el mismo archivo | `Objects/RenderTexture.cpp:225-236` vs `:318-329` | Helper estático local único |
| G11 | `TextureObject::create_func` vs `create2_func` (~95% idénticos) | `Objects/Texture.cpp:721-833` vs `:835-947` | Interno `CreateTextureVkImage(...)` compartido |
| G12 | `RenderTextureObject::create_func` vs `create2_func` | `Objects/RenderTexture.cpp:212-303` vs `:305-393` | Igual que G11 |
| G13 | `get_swizzle`/`CheckFormat`/`CheckSwizzle` | `Objects/Texture.cpp:92-173` vs `Objects/StorageTexture.cpp:44-172` | Versión de Texture (superconjunto) |
| G14 | Formato de vertex input: tabla VkFormat + component count | `Shader.cpp:137-147` vs `GraphicsRender.cpp:2212-2278` (`get_input_format`, con `case 22` duplicado interno) | Extender `Shader.cpp` con `ShaderGen5VertexInputVkFormat` |
| G15 | Registros SH directo vs indirecto | `GraphicsRun.cpp:2514-2809` vs `:4907-5010+` | Parsers directos `hw_sh_set_*` |

### Riesgo BAJO

- `rt_print` (`GraphicsRender.cpp:758-809`) duplica el conocimiento de campos de los parsers PM4 — solo debug.
- Decode `dst_sel` de SDWA repetido: `ShaderParse.cpp:973-986` vs `:1154-1170`.
- Proliferación de probes `KYTY_DUMP_*` (5 env-vars, lógica solapada de captura).

### Patrones positivos que ya existen (imitar, no reinventar)

- Delegación a `GraphicsState::Set*` desde ambos caminos PM4 (scissor/blend).
- `TileGet*` como motor de layout único.
- `UtilBlitImage` / `UtilImageToImage` como única ruta de blit/copy.

---

## 2. Hallazgos — runtime / HLE / scripts

### Riesgo ALTO

| # | Responsabilidad | Copias | Canónico |
|---|-----------------|--------|----------|
| R1 | Colisión de NID entre librerías (misma NID, semántica distinta; gana el último `InitAll`) | `Ec63y59l9tw`: `LibNet.cpp:156` vs `LibAudio.cpp:63` · `Gz1rmUZpROM`: `LibNpTrophy2.cpp:60` vs `LibAudio.cpp:66` · `jbz9I9vkqkk`: `LibC.cpp:1897` vs `LibAudio.cpp:61` | Implementación específica de dominio; eliminar los binds catch-all `AudioOut2LogAndOk` sobre NIDs ajenas |
| R2 | Registro doble de bloques init | `Libs.cpp:116-157`: `InitLibcInternal_1` llamado 2×; `InitNpManager_1` llamado 2× (líneas 134 y 136) | Un solo pase de `InitAll`; cada `LIB_DEFINE` una vez |
| R3 | Re-bind de la misma NID dentro de LibC (`8zTFvBIAIN8` memset, `eLdDw6l0-bU` snprintf, `H2e8t5ScQGc`/`tsvEmnenz48` cxa) | `LibC.cpp:1693-1698` vs `:1800-1817`, `:1884` | Implementaciones `LibC::c_*` |
| R4 | Helpers Python copiados entre runners (`kill_process_group`, `agent_call` — ya divergen en timeout 5s vs 8s y buffer 64KB vs 256KB) | `scripts/kyty_games_matrix.py:315-379` vs `scripts/kyty_playable_regression.py:183-237`; `kyty_capture.py:466,513` repite el launch | Módulo compartido `scripts/kyty_runner_common.py` |
| R5 | `VirtualMemory::Init` llamado desde dos subsistemas y `sys_virtual_init` no es idempotente | `lib/Core/src/Core.cpp:49` y `Kernel/Memory.cpp:195` → `SysLinuxVirtual.cpp:74-92` | Init solo desde `Core`; `Memory` asume VM lista (o guard de idempotencia) |

### Riesgo MEDIO

| # | Responsabilidad | Copias | Canónico |
|---|-----------------|--------|----------|
| R6 | NIDs duplicadas dentro de LibKernel (Pthread vs Posix): `FXPWHNk8Of0`, `7H0iTOciTLo`, `lLMT9vJAck0`, `HoLVWNanBBc` | `LibKernel.cpp:612-654`, `768-796`, `843-889` | Una NID → un handler; wrappers `Posix::` finos |
| R7 | Registro SaveDataDialog "native" copiado en vez de reusar helper | `LibDialog.cpp:33-39` vs `:58-63` (MsgDialog ya lo hace bien en `:103`); `LibSaveData.cpp:746-799` similar, más NID doble `WAzWTZm1H+I`/`RjMlsR8EXrw` → misma función en `:768-769` | Helper `Register*Funcs` por variante |
| R8 | Mapeo de heap guest por dos rutas (syscall map vs demand-range directo) | `LibKernel.cpp:354-384` vs `Kernel/Memory.cpp:347-390` + `VirtualMemory.cpp:462-496` | API demand de `VirtualMemory`; HLE como capa fina |
| R9 | Dos clases `EventRing` con el mismo nombre | `Emulator/Agent/EventRing.h` vs `lib/DevTools/.../Telemetry/EventRing.h` | Mantener separadas pero renombrar una (colisión de nombre, no de comportamiento) |
| R10 | Vías de diagnóstico solapadas (BringUp / AgentLifecycle / FAULT_LOG / printf de MissingImport) | `BringUp.cpp`, `AgentLifecycle.cpp:104-125`, `VirtualMemory.cpp:184-191,615-625`, `MissingImport.cpp:75-76` | BringUp = política; AgentLifecycle = eventos; FAULT_LOG = solo señal |

### Riesgo BAJO / intencionales (NO centralizar)

- Alias multi-NID → una función (`c_memcpy`, `cxx_new`, etc.): diseño correcto.
- `GuestCall::Invoke`: trampoline único, sin duplicado.
- `MissingImportStubs` / `NeighborModulePreload`: ya eliminados, sin restos.
- `kyty_games_matrix.py`: una sola copia (el integration test la importa).
- Asignadores de heap paralelos (libc passthrough / mspace / ApplicationHeap): contratos guest distintos, deben coexistir documentados como alternativas explícitas.

---

## 3. Plan de centralización (por fases, un seam por commit)

Precondición global (AGENTS.md): no iniciar extracciones mientras haya un
bloqueador estricto post-Play abierto sin congelar; cada paso es
**behavior-neutral**, con test de caracterización antes de mover código, y
borrando la copia vieja en el mismo commit (sin aliases permanentes).

### Fase 0 — Congelar comportamiento (sin mover nada)

1. Test de caracterización PM4: mismo stream de registros por camino directo
   e indirecto debe producir `HW::Context` idéntico (cubre G1–G4, G15).
2. Test de tabla de formatos: para cada `(dfmt,nfmt,fmt)` soportado,
   Texture y StorageTexture devuelven el mismo VkFormat donde ambos aplican (G6).
3. Test Python: `agent_call`/`kill_process_group` con socket fake — fija el
   contrato actual antes de extraer (R4).

### Fase 1 — Correcciones de riesgo alto que son bugs latentes

1. **R1** Colisiones de NID entre libs: quitar binds `AudioOut2LogAndOk`
   sobre NIDs de Net/NpTrophy2/LibC. Riesgo de regresión: verificar con
   matrix que ningún título dependía del orden actual.
2. **R2/R3** Registro doble en `InitAll`/LibC: dejar cada init una vez.
3. **G4** Anomalía `buffer[1]` en `hw_ctx_set_stencil_info`: confirmar con
   captura PM4 real y test rojo antes de tocar.
4. **R5** Idempotencia de `sys_virtual_init` (guard) + un solo caller.

### Fase 2 — Unificar decodificación PM4 (mayor superficie)

Orden: G2 → G3 → G4 → G1 → G15, un registro por commit.
Cada commit: extraer helper de campos, llamarlo desde directo + indirecto +
batched, borrar los cuerpos inline, correr GraphicsPackets/GraphicsState y
un replay estricto corto.

### Fase 3 — Unificar tablas de formato/layout

Orden: G6 → G14 → G7 → G8 → G10.
Regla: toda tabla `formato → VkFormat/BPE/pitch` vive junto a las existentes
en `Shader.cpp`/`Tile.cpp` (o un `GraphicsFormat.h` nuevo si crece), con un
solo punto de fallo estructural para formatos no soportados.

### Fase 4 — Deduplicar creación de objetos y dumps

- G11/G12: interno compartido de creación de VkImage por objeto.
- G9: todos los dumps BMP pasan por `UtilDumpVulkanImageRgba8Bmp`.
- G5: el pre-scan de fences llama a los mismos helpers que el CP.

### Fase 5 — Runtime/HLE y scripts

- R6/R7: limpiar NIDs duplicadas intra-LibKernel y helpers de registro de diálogos.
- R8: HLE de mapeo de heap sobre la API demand única.
- R4: extraer `scripts/kyty_runner_common.py` e importarlo desde matrix,
  playable y capture (con sus tests).
- R9: renombrar uno de los dos `EventRing`.

### Criterio de "hecho" por paso

1. Test de caracterización verde antes y después.
2. `ninja -C _build_linux` limpio.
3. Replay estricto corto sin retroceso del frontier.
4. La implementación vieja eliminada en el mismo commit (cero forwarding).

---

## 4. Qué NO se debe centralizar

- Encode (`Graphics.cpp`) vs decode (`GraphicsRun.cpp`) de PM4: es el par
  productor/consumidor esperado, no duplicación.
- Los dos `EventRing`: comportamiento distinto (mutex/512 vs SPSC/256);
  solo el nombre colisiona.
- CLI del agente vs servidor: cliente/servidor legítimos; compartir solo el
  esquema (`WireContract`/`Protocol`).
- Los tres asignadores guest (libc host, mspace, ApplicationHeap):
  contratos guest diferentes.
- Alias multi-NID de una misma función.
