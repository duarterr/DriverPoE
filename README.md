# DriverPoE

Firmware para uma luminária LED alimentada por PoE. Ele roda em um ESP32, recebe energia por um TPS2378, controla o driver de LED HV9910 e se conecta à rede Ethernet por um PHY IP101G.

## O que faz

- Confirma que a alimentação PoE ou auxiliar está estável antes de liberar o LED.
- Mede a tensão do barramento e do LED.
- Liga, desliga e ajusta o brilho com rampa suave.
- Salva na NVS o último estado ligado/desligado e o último brilho usado.
- Obtém IP por DHCP na Ethernet, anunciando-se com o hostname "DriverPoE" (em vez do padrão "espressif" do ESP-IDF) na lista de clientes do roteador.
- Permite controle local e remoto por um canal UDP autenticado (`tools/lumtool.py`, ou a interface web em `tools/webui/`).
- Recebe atualizações de firmware (OTA) pelo mesmo canal UDP administrativo, com verificação SHA-256 da imagem antes de aplicar.

O produto é Ethernet/PoE puro: não usa Wi-Fi, Bluetooth nem Matter. O único protocolo de controle é o canal UDP administrativo próprio descrito abaixo.

## Como funciona

No boot, o driver de LED começa desligado. O firmware monitora os sinais do TPS2378 e a tensão VBUS. Só considera a alimentação válida quando há uma fonte detectada e o barramento está acima de 40 V. Se a alimentação cair, desliga o driver imediatamente.

A Ethernet só é iniciada depois que a alimentação é confirmada; a partir daí o firmware inicia o canal de administração UDP. Caso o LED estivesse ligado antes de um reinício, ele volta a ligar (com o mesmo brilho) assim que a alimentação for confirmada — inclusive quando esse "ligar" foi pedido remotamente enquanto a alimentação ainda não estava pronta (ver "Canal de administração" abaixo). `INFO` sempre reporta o motivo exato de um eventual bloqueio (CDB/T2P/VBUS), mesmo que hoje só seja alcançável depois que a energia já foi confirmada.

| Indicador | GPIO | Significado |
| --- | --- | --- |
| Azul | GPIO12 | Pisca enquanto a alimentação não foi confirmada; fica aceso quando o VBUS está válido. |
| Vermelho | GPIO14 | Aceso quando o driver HV9910 está habilitado. |

## Canal de administração

Protocolo binário próprio sobre UDP, porta `5001` (`ADMIN_UDP_PORT`), versão `4` (`ADMIN_PROTO_VERSION` em `components/admin_channel/admin_protocol.h`).

**Modelo de segurança**: cada unidade tem um segredo de 32 bytes (`components/devid/devid.h`), gravado automaticamente na NVS com um valor padrão de fábrica documentado em claro (`ADMIN_DEFAULT_SECRET`, em `main/poe_luminaire_main.h`) no primeiro boot — e de novo após qualquer `FACTORY_RESET`, que apaga a mesma partição NVS onde o segredo mora. `INFO` é o único comando sem autenticação (leitura pura, sem efeito colateral, respondida a qualquer um — inclusive broadcast). Todo comando que muda algo (`ON`/`OFF`/`DIM`/`IDENTIFY`/`REBOOT`/`FACTORY_RESET`/`CHANGE_SECRET`) exige HMAC-SHA256 com o segredo ativo **e** um nonce de uso único obtido via `CHALLENGE` imediatamente antes, vinculado ao IP de origem e com expiração curta (5 s) — protege contra repetição de um pacote capturado, não só contra falsificação. `CHANGE_SECRET` troca o segredo diretamente (payload cifrado com AES-256-GCM sob o segredo atual), sem etapa de confirmação separada.

O segredo padrão é conhecido (está neste repositório) — a segurança real desta camada vem de trocá-lo durante a instalação, não de mantê-lo em segredo. Trate-o como a senha padrão impressa embaixo de um roteador doméstico.

**Semântica de `ON`/`DIM`**: o canal admin só começa a escutar depois que a alimentação já foi confirmada, então na prática isso raramente é atingido — mas se um `ON` ou `DIM>0` chegar num momento em que `tps2378_is_ready()` está falso (ex.: a alimentação caiu e ainda não voltou), o comando é aceito e a intenção é persistida mesmo assim (sem nunca ligar o driver fora do gate elétrico do TPS2378) — a resposta usa o status `ACCEPTED_PENDING`, distinto de `OK`, para deixar isso explícito ao cliente. O LED liga sozinho, no brilho pedido, assim que a alimentação for confirmada (ou reconfirmada). `OFF`/`DIM 0` sempre se aplicam e persistem imediatamente, independente do estado da alimentação.

`FACTORY_RESET` apaga toda a partição NVS do equipamento: brilho/estado ligado-desligado salvos e o segredo administrativo (que volta ao padrão de fábrica).

### Ferramentas de host

Toda a lógica de protocolo/HMAC/AES-GCM/descoberta mora em um único pacote Python puro, `tools/driverpoe/` (sem `input()`/`print()`/efeitos colaterais fora da rede) — `tools/lumtool.py` e `tools/webui/` são consumidores desse pacote, não reimplementações paralelas dele.

**`tools/driverpoe/`** — API Python:

| Módulo | Conteúdo |
| --- | --- |
| `protocol.py` | Constantes, `Packet` (serialização/HMAC), identidade (MAC/serial), `parse_info_payload()`. |
| `client.py` | `AdminClient` (um socket UDP por unidade; `info/challenge/on/off/dim/identify/reboot/factory_reset/change_secret`), exceções tipadas (`DeviceTimeoutError`, `AuthError`, `ProtocolError`/`ProtocolVersionMismatchError`, `CommandRefusedError`, `MissingDependencyError`), `find_working_secret()`/`connect()`. |
| `discovery.py` | `broadcast_info()`, `resolve_device_by_ip()`, `guess_broadcast_address()`. |
| `models.py` | `DeviceInfo` (com `power_blocking_reason`), `CommandResult` (`accepted`/`applied`/`pending`, `raise_if_refused()`). |
| `secrets.py` | `SecretStore` (protocolo mínimo: `get/set/delete`), `JsonFileSecretStore` (`tools/admin_secrets.json`, gitignored), `MemorySecretStore`. |
| `cli.py` | O menu interativo — único lugar do pacote com I/O de terminal. |

```powershell
python -c "from driverpoe import discovery, connect, JsonFileSecretStore; d=discovery.broadcast_info(discovery.guess_broadcast_address()); print(d)"
```

**`tools/lumtool.py`** — CLI fina sobre o pacote acima (menu interativo: escanear, selecionar, controlar, administrar):

```powershell
python tools/lumtool.py
```

**`tools/webui/`** — interface web local (FastAPI + HTML/JS simples, sem framework de frontend), consumindo exclusivamente o pacote `driverpoe` no backend; o segredo admin nunca é enviado ao navegador exceto uma vez, de volta ao operador, logo após um `CHANGE_SECRET` bem-sucedido que ele mesmo pediu:

```powershell
pip install -r tools/webui/requirements.txt
Set-Location tools
python -m webui.app   # ou: uvicorn webui.app:app --reload
```

Abra `http://127.0.0.1:8000/`. É uma ferramenta local sem autenticação própria (mesma postura do canal UDP: qualquer um que alcance a interface consegue mandar comandos autenticados) — não exponha fora de uma rede de gerência confiável.

## Configuração da placa

Os parâmetros específicos de hardware ficam em [main/poe_luminaire_main.h](main/poe_luminaire_main.h): GPIOs, polaridades, endereço do PHY, divisor de tensão, limite de VBUS e porta UDP. Ajuste esse arquivo antes de compilar para outra revisão de hardware. `DEVID_MODEL_PREFIX` (hoje "DriverPoE") é a única fonte do nome do produto — prefixa tanto o serial (`devid_get_serial()`) quanto o hostname anunciado no DHCP (`eth_init`'s `hostname` config); mudar esse `#define` atualiza os dois automaticamente.

Configuração principal desta placa:

| Item | Valor |
| --- | --- |
| MCU | ESP32 |
| Ethernet | EMAC interno, RMII, PHY IP101G (endereço 1) |
| PoE | TPS2378; IEEE 802.3af/at ou alimentação auxiliar |
| Driver de LED | HV9910, PWM de 10 kHz |
| VBUS mínima | 40 V |
| Flash | 4 MB, duas partições OTA |

## Build e gravação

O projeto usa ESP-IDF `6.0.2`. A dependência `ip101` (driver do PHY) é baixada pelo Component Manager na primeira compilação. É necessário ter o ambiente do ESP-IDF carregado.

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM_X flash monitor
```

Substitua `COM_X` pela porta serial da placa.

Para executar os testes das ferramentas de host (protocolo, cliente, descoberta, modelos, segredos):

```powershell
Set-Location tools
python -m unittest discover -s driverpoe/tests -v
```

## Estrutura

| Caminho | Responsabilidade |
| --- | --- |
| `main/` | Inicialização e integração dos componentes. |
| `components/hv9910/` | Controle do brilho e do estado do driver LED. |
| `components/tps2378/` | Detecção de PoE/AUX e validação da alimentação. |
| `components/voltage_sense/` | Leitura de VBUS e tensão do LED pelo ADC. |
| `components/eth_init/` | Ethernet RMII e DHCP. |
| `components/admin_channel/` | Protocolo UDP autenticado. |
| `components/devid/` | Serial, MAC e segredo administrativo. |
| `components/status_leds/` | LEDs de status da placa. |
| `tools/driverpoe/` | Pacote Python: protocolo, cliente, descoberta, modelos, segredos. |
| `tools/lumtool.py` | CLI de descoberta, controle e administração, sobre `tools/driverpoe/`. |
| `tools/webui/` | Interface web local (FastAPI), sobre `tools/driverpoe/`. |

## Atualização OTA

A imagem é transferida pelo **mesmo canal UDP administrativo** (`ADMIN_TYPE_OTA_BEGIN/OTA_CHUNK/OTA_END/OTA_ABORT`, versão de protocolo `4`) — sem cliente HTTP no firmware, sem nova superfície de rede. Fluxo:

1. `OTA_BEGIN` (HMAC + nonce) anuncia o tamanho total e o SHA-256 da imagem completa.
2. `OTA_CHUNK` (HMAC, sem nonce — ver o comentário de `needs_pool_nonce` em `admin_channel.c` para o porquê) envia a imagem em pedaços de até 1024 bytes, cada um confirmado com o total já gravado — perder uma resposta é seguro, o cliente só reenvia o mesmo chunk.
3. `OTA_END` (HMAC + nonce) confere que todos os bytes chegaram, confere o SHA-256 da imagem inteira e só então valida (`esp_ota_end`) e marca a partição como próxima de boot. Qualquer falha aborta sem tocar na partição atual.
4. A imagem nova só roda depois de um `REBOOT` explícito — nunca automaticamente. A partir daí, a confirmação de rollback já existente (`confirm_app_if_pending_verify()` em `main.c` + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) garante que uma imagem que não confirma sozinha volta para a anterior.

Uma sessão de OTA parada (sem `OTA_CHUNK` por 30s) é descartada automaticamente, então uma transferência abandonada nunca trava o canal permanentemente para uma tentativa seguinte.

**Pela interface web**: cada card tem um campo de arquivo + botão "Upload firmware" — escolha o `.bin` (ex.: `build/driverpoe.bin`) e envie; uma barra de progresso acompanha a transferência em tempo real, e ao final a página oferece reiniciar a unidade para aplicar.

**Pelo pacote Python**: `AdminClient.ota_update(secret, serial, image_bytes, progress_callback=...)`, em `tools/driverpoe/client.py`.
