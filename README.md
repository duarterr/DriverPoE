| Supported Targets | ESP32 |
| ----------------- | ----- |

# Luminária PoE — firmware mínimo

Firmware mínimo para uma luminária alimentada por PoE, construído sobre:

- **ESP32** (EMAC interno)
- **IP101G** — PHY Ethernet via RMII
- **TPS2378** — negociador/interface PoE (IEEE 802.3at, PD)
- **HV9910** — driver de LED (buck, dimerização por PWM)

## Comportamento

1. No boot, antes de qualquer outra inicialização, o driver de LED HV9910
   é forçado ao estado desabilitado (`hv9910_init()`).
2. O ADC (`voltage_sense_init()`) sobe em seguida, para que o VBUS já
   esteja legível assim que o gate de PoE/AUX começar a avaliá-lo.
3. O firmware passa a monitorar os pinos **CDB** e **T2P** do TPS2378,
   cruzados com a tensão medida do barramento DC (**VBUS**, lida via
   `ADC_VLED_P`). O sistema é considerado "pronto" (seguro para operar)
   quando **ambos**:
   - Uma fonte digital é confirmada — **ou**:
     - **PoE OK** — CDB confirma que uma fonte PoE real foi negociada e o
       inrush terminou (vale tanto para Type-1/802.3af quanto
       Type-2/802.3at), ou
     - **AUX presente** — T2P está ativo, o que acontece tanto numa
       classificação real Type-2 quanto porque a entrada de alimentação
       auxiliar de bancada desta placa (injeção DC direta no barramento a
       jusante, para testes sem uma fonte PoE real) está acima de ~40V e
       força o pino APD do TPS2378 para alto.
   - **E o VBUS confirma** — a tensão DC medida do barramento está acima
     de `VBUS_MIN_MV` (padrão 40V, `components/poe_luminaire/poe_luminaire.h`). CDB/T2P são só
     sinais digitais de handshake; VBUS é a checagem final de que o
     barramento está de fato saudável antes de confiar o driver de LED
     pra rodar.

   Enquanto **qualquer** um dos requisitos falhar ("modo de baixo
   consumo"), o **LED indicador 2 (vermelho, GPIO12)** pisca e o driver de
   LED fica travado — inclusive se a condição combinada cair de novo
   depois de já ter estado OK (perda de energia, AUX removido, VBUS
   caindo, sobrecarga térmica, renegociação), o firmware corta o driver
   imediatamente. Veja [components/poe_luminaire/poe_negotiator.h](components/poe_luminaire/poe_negotiator.h) para
   a lógica completa e o raciocínio.
4. Assim que o sistema fica pronto, o firmware:
   - inicializa o PHY IP101G e sobe a pilha Ethernet (cliente DHCP por
     padrão, via `esp_netif`/lwIP);
   - inicia um servidor de comandos TCP (porta configurável, padrão 5000);
   - inicia o canal de administração UDP autenticado (porta configurável,
     padrão 5001) — ver seção própria abaixo.
5. A partir daí, comandos recebidos pela rede podem ligar/desligar o
   driver, ajustar o brilho e ler tensões/estado.

Veja [components/poe_luminaire/poe_luminaire.h](components/poe_luminaire/poe_luminaire.h) para o mapa de pinos completo
e para **todo parâmetro de configuração da aplicação** (porta TCP,
polaridades do HV9910, frequência de PWM, endereço do PHY, razão do
divisor de tensão) — tudo como `#define`s, deliberadamente sem nada no
Kconfig/menuconfig. Pra mudar qualquer um desses valores, edite
`poe_luminaire.h` e recompile.

## Relato de energia PoE / AUX

Uma vez pronto, o firmware registra e reporta (via `STATUS`, campo
`poe_source`) qual condição digital foi satisfeita e — para as duas
fontes PoE reais — a potência garantida disponível no PD, usando os
números-padrão do IEEE 802.3af/at. Isso **não** é uma medição de potência
ao vivo:

| `poe_source` | Significado                                    | `poe_power_w`        |
| ------------- | ------------------------------------------- | ---------------------|
| `none`        | Modo de baixo consumo: nenhuma fonte digital confirmada, ou VBUS baixo demais | `0.00` |
| `type1`       | PoE real negociado, IEEE 802.3af           | `12.95`               |
| `type2`       | PoE real negociado, IEEE 802.3at (Type-2)  | `25.50`               |
| `aux`         | Fonte auxiliar de bancada presente, CDB não confirmado | `0.00` (não é um orçamento PoE real) |

Note que `type2` não dá pra distinguir em software de "PoE Type-1 com a
fonte AUX também conectada" — os dois deixam CDB confirmado (PoE real) e
T2P ativo (AUX forçando APD). De qualquer forma é seguro operar, que é
tudo que o firmware precisa saber; a classe reportada nesse caso de
sobreposição é só um rótulo de melhor esforço, não uma garantia de
classificação 802.3at real.

`poe_source` reflete só o handshake digital — `poe_ready` exige
adicionalmente que o VBUS esteja acima de `VBUS_MIN_MV` (`vbus_ok=1` em
`STATUS`). É possível ver um `poe_source` diferente de `none` junto com
`poe_ready=0`: significa que CDB/T2P afirmam que há energia, mas a tensão
medida do barramento não confirma isso (barramento caindo, falha de
fiação, etc) — confira `vbus_mv` nesse caso.

## Protocolo de comandos (TCP, texto)

Uma linha de texto por comando, terminada por `\n` (um `\r` opcional logo
antes é ignorado). Conecte com `netcat`/`telnet`/`ncat` na porta
configurada (padrão `5000`):

```
$ nc <ip-da-luminaria> 5000
PING
OK PONG
STATUS
OK STATUS poe_ready=1 poe_source=type2 poe_power_w=25.50 cdb_raw=1 cdb_ok=1 t2p_raw=1 t2p_ok=1 vbus_ok=1 driver_on=0 dim=0 dim_last=50 vbus_mv=48200 led_voltage_mv=3100 eth_ip=192.168.1.50 uptime_s=42
ON
OK ON 250ms
DIM 100 100
OK DIM 100 100ms
DIM 0 0
OK DIM 0 0ms
OFF
OK OFF 250ms
```

| Comando                  | Efeito                                                                |
| ------------------------- | ---------------------------------------------------------------------|
| `PING`                    | `OK PONG`                                                             |
| `HELP`                    | Lista de comandos                                                     |
| `STATUS`                  | Estado atual: fonte e potência PoE/AUX, CDB/T2P/VBUS crus e debounced, driver, dimerização, tensões, IP |
| `ON [rampa_ms]`           | Habilita o HV9910, subindo até o último brilho ao longo de `rampa_ms` milissegundos (padrão: rampa da placa; recusado com `ERR POE_NOT_READY` a menos que PoE ou AUX esteja confirmado) |
| `OFF [rampa_ms]`          | Desce o brilho até apagar ao longo de `rampa_ms` milissegundos (padrão: rampa da placa; `0` = instantâneo) |
| `DIM <0-100> [rampa_ms]`  | Faz a transição de brilho (%) até o alvo ao longo de `rampa_ms` milissegundos (inteiro, ex. `100`). Se omitido, usa a rampa padrão da placa (`HV9910_DEFAULT_RAMP_MS`, 250ms); `0` = instantâneo. `DIM 0` também desabilita o driver assim que a rampa termina; `DIM >0` também habilita se estava desligado (mesmo gate de PoE/AUX do `ON`) |

Campos do `STATUS` explicados:

| Campo | Significado |
| ----- | ------- |
| `cdb_raw` / `t2p_raw` | Leituras instantâneas de GPIO, sem debounce — podem mostrar glitches transitórios |
| `cdb_ok` / `t2p_ok`   | Valores com debounce (confirmados) — no que `poe_source` realmente se baseia |
| `vbus_ok`             | Veredito com debounce de "VBUS acima do limiar" — `poe_ready = (cdb_ok ou t2p_ok) e vbus_ok` |
| `vbus_mv`             | Leitura ao vivo da tensão do barramento DC, em mV (= LEDVP escalado) — compare com `VBUS_MIN_MV` em `components/poe_luminaire/poe_luminaire.h` |
| `dim`                 | Brilho atual ao vivo (0 enquanto o driver está desligado) |
| `dim_last`            | Brilho lembrado que `ON`/`DIM >0` vai retomar, persistido em NVS |

O servidor atende uma conexão por vez (mínimo, de propósito).

## Persistência de brilho e estado ligado/desligado

Dois valores são salvos na NVS (namespace `hv9910`) toda vez que mudam,
e recarregados no boot:

- **Brilho** (chave `dim`) — o último brilho não-zero, atualizado a cada
  `DIM`. Um dispositivo novo, sem nada salvo ainda, usa 100% por padrão.
- **Estado ligado/desligado** (chave `on`) — se o driver foi deixado
  ligado ou desligado da última vez. Um dispositivo novo, sem nada salvo
  ainda, é tratado como desligado (nunca liga sozinho numa unidade
  virgem).

### API simples, três funções

`components/poe_luminaire/hv9910.c` expõe só três funções pra tudo:

| Função | Faz |
|---|---|
| `hv9910_enable(ramp_ms, persist)` | Libera o `SHUTDOWN` e sobe até o brilho lembrado, ao longo de `ramp_ms` |
| `hv9910_disable(ramp_ms, persist)` | Desce o brilho até 0 ao longo de `ramp_ms`, só então trava o `SHUTDOWN` |
| `hv9910_set_dim(percent, ramp_ms)` | Muda só o brilho, não mexe no `SHUTDOWN` |

`ramp_ms = 0` em qualquer uma das três significa instantâneo. O `persist`
de `enable`/`disable` controla só se o novo estado ligado/desligado é
gravado na NVS (chave `on`, namespace `hv9910`) — `true` quando é
intenção real do operador (`ON`, `OFF`, `DIM` ligando/desligando o
driver, que deve sobreviver a reinícios e quedas de energia); `false`
para mudanças transitórias que não devem redefinir "o que o operador
quer" — o corte de segurança do `poe_negotiator` quando a energia cai, o
religamento automático quando ela volta, e o pisca-pisca temporário do
`IDENTIFY`.

**Por que o `persist` importa de verdade**: se o corte de segurança por
queda de PoE gravasse "desligado" toda vez que dispara, o religamento
automático nunca funcionaria depois de uma queda de energia real — a
própria queda apagaria a memória de "estava ligado" um instante antes de
precisar dela. Por isso `poe_negotiator.c` chama
`hv9910_disable(0, false)` no corte (instantâneo, não grava) e
`hv9910_enable(HV9910_DEFAULT_RAMP_MS, false)` no religamento (rampa
padrão, não grava — só está restaurando um estado que já foi gravado
antes, não criando intenção nova).

`hv9910_enable()` sempre reaplica o brilho lembrado (chave `dim`) antes
de liberar o `SHUTDOWN`, então o driver nunca volta apagado. Um
dispositivo novo, sem nada salvo ainda, usa 100% de brilho por padrão e
começa desligado (nunca liga sozinho numa unidade virgem). Veja
[components/poe_luminaire/hv9910.h](components/poe_luminaire/hv9910.h) para a API completa e o raciocínio de cada
função. Isso usa a partição `nvs` já declarada em
[partitions.csv](partitions.csv).

## Rampas de brilho

Toda rampa é feita pelo próprio hardware do LEDC
(`ledc_set_fade_with_time()` + `ledc_fade_start()`, modo
`LEDC_FADE_NO_WAIT`) — não é um laço de software indo passo a passo,
então não bloqueia a task que chamou nem consome CPU durante a rampa.
`ramp_ms` é sempre em milissegundos, a unidade nativa do periférico —
inclusive na porta de texto (`DIM <0-100> [rampa_ms]`, `ON [rampa_ms]`,
`OFF [rampa_ms]`), sem conversão nenhuma no meio do caminho. Se omitida,
usa `HV9910_DEFAULT_RAMP_MS` (`components/poe_luminaire/poe_luminaire.h`, `250`).

`IDENTIFY` (canal admin) continua piscando com rampa zero (instantâneo)
nas duas transições, de propósito — o objetivo ali é um pisca-pisca
nítido pra identificação física, não uma transição suave.

### ⚠️ Limitação de hardware: o ESP32 clássico não cancela uma rampa em andamento

O periférico LEDC do ESP32 "clássico" (o chip usado nesta placa) **não
tem `ledc_fade_stop()`** — só chips mais novos (S2/S3/C3/C6...) suportam
cancelar ou reapontar uma rampa já iniciada. Chamar
`ledc_set_fade_with_time()`/`ledc_fade_start()` enquanto uma rampa
anterior no mesmo canal ainda não terminou **bloqueia a chamada** até
essa rampa anterior acabar (ela segura um semáforo interno) — e quando
finalmente roda, o duty já chegou no alvo da primeira rampa, então a
segunda não tem mais nada a fazer. Na prática: **duas rampas disparadas
em sequência no mesmo canal, sem esperar a primeira acabar, fazem a
segunda "sumir"** — foi exatamente o bug que causou `DIM 100 <qualquer
rampa>` parecer instantâneo quando o driver estava desligado, numa
versão anterior deste código.

A correção é nunca disparar duas rampas seguidas no mesmo canal, sem
precisar de nenhuma função extra: quando o `DIM` do `cmd_server.c`
precisa ligar o driver a partir de desligado com um alvo específico, ele
chama `hv9910_enable(0, true)` — `ramp_ms=0` faz isso passar pelo
caminho instantâneo (que nunca toca o hardware de fade), então não
compete com a rampa de verdade que vem logo em seguida via
`hv9910_set_dim(valor, rampa_ms)`. Só uma rampa é disparada por comando,
sempre.

## Identidade do dispositivo

### Por que tudo isso existe

Essa luminária, depois de instalada, fica no teto — sem tela, sem botão,
sem porta serial acessível. Qualquer coisa que precise fazer com ela
depois da instalação (saber que unidade é essa, checar o estado dela,
reiniciar, resetar de fábrica, recuperar o acesso se alguma coisa der
errado) só pode acontecer **pela rede**. Isso cria um problema que uma
luminária de bancada, ligada num cabo serial, nunca tem: como ter certeza
de que quem está mandando um comando "reinicie" ou "apague tudo" pela
rede é realmente você, e não qualquer outro dispositivo no mesmo Wi-Fi ou
cabo de rede? Sem alguma forma de autenticação, **qualquer um** que
alcançasse a luminária pela rede poderia reiniciá-la, apagar a
configuração dela, ou pior — e não teria como saber que aconteceu até
notar a luz apagada.

Cada peça do que vem a seguir resolve uma parte específica desse
problema:

- **O serial** existe porque você vai ter várias dessas luminárias na
  mesma rede, e precisa de um jeito de dizer "essa aqui, não aquela
  outra" — o IP sozinho não serve, porque DHCP pode trocar o IP de uma
  unidade a qualquer momento. O serial precisa ser único e nunca mudar; daí
  ele vir do MAC (que a Espressif já grava de fábrica em cada chip, único
  por definição) em vez de ser inventado ou sorteado.
- **A chave de administração** existe porque comandos como `REBOOT` ou
  `FACTORY_RESET` são poderosos demais pra ficarem abertos pra qualquer um
  na rede. Cada unidade tem a **sua própria** chave — não uma senha
  compartilhada entre todas — porque, se fosse uma senha só pra todo mundo,
  vazar ou adivinhar essa senha uma vez daria controle sobre **toda** a
  frota de luminárias de uma vez. Com uma chave por unidade, o pior caso de
  alguém descobrir a chave de uma luminária é só aquela luminária.
- **O segredo mestre + a fórmula de derivação (HKDF)** existem porque
  "uma chave por unidade" cria um problema novo: como *você* (o
  administrador legítimo) lembra a chave de centenas de luminárias
  diferentes, sem manter um banco de dados frágil que, se perdido, te
  deixa sem conseguir administrar nada? A resposta é não guardar chave
  nenhuma: a chave de qualquer unidade é sempre **recalculável na hora**,
  a partir de só três coisas — o segredo mestre (que só você tem), o MAC
  da unidade (público, está no próprio serial) e a época atual (que a
  própria unidade informa via `DISCOVER`). Nada pra fazer backup, nada pra
  perder.
- **A `epoch`** existe pra permitir trocar a chave de uma unidade
  (rotação, por segurança, ou depois de uma suspeita de comprometimento)
  sem precisar inventar um mecanismo novo — é só uma versão da mesma
  fórmula (detalhes na próxima seção).
- **O comando `CLAIM`** existe pra resolver o primeiro instante da vida
  de uma unidade: ela sai de fábrica sem chave de administração nenhuma
  (a chave real só existe depois que alguém com o segredo mestre a
  calcula). Ele é **deliberadamente não autenticado** — não existe nada
  pra proteger ainda, já que a unidade não tem identidade nenhuma nesse
  momento. A proteção real não é criptográfica: é que `CLAIM` só é aceito
  **uma vez**, enquanto `devid_is_provisioned() == false`; assim que uma
  unidade recebe seu primeiro `CLAIM` válido, ela passa a recusar
  qualquer `CLAIM` seguinte pra sempre — não há como "readotar" ou
  sobrescrever uma identidade já gravada (mais detalhes mais abaixo).
- **`FACTORY_RESET` e o resto dos comandos de recuperação remota**
  existem porque, sem acesso físico, a única forma de tirar uma luminária
  de um estado ruim (configuração travada, aplicação com bug) é pela
  rede — e como qualquer comando remoto poderoso, também precisa da
  autenticação acima pra não virar uma porta aberta.

Com isso resolvido, os detalhes técnicos de cada peça:

Cada unidade recebe um **serial** determinístico
(`"LUM1-<12 hex do MAC base>"`, ex. `LUM1-A4CF12B93D08`) e uma **chave de
administração** de 32 bytes, ambos tratados por `components/poe_luminaire/devid.h`/`components/poe_luminaire/devid.c`:

- O serial **nunca é gravado na flash** — é recalculado a cada boot a
  partir do MAC base do eFuse (`esp_efuse_mac_get_default()`), então não
  há nada pra provisionar em relação a ele.
- A chave de administração é **derivada, não aleatória**:
  `HKDF-SHA256(ikm=segredo mestre, salt=MAC base, info="lum-admin-v1" ||
  modelo || epoch)`. Essa derivação só roda fora do dispositivo — só em
  `tools/lumtool.py` (rodando em Python no PC, tanto pra provisionar
  quanto pra administrar), usando só `hashlib`/`hmac` da biblioteca
  padrão. O firmware nunca vê o segredo mestre e nunca deriva
  nada; ele só armazena a chave já derivada mais a época (`epoch`) atual,
  numa partição NVS dedicada (`idnvs`, namespace `devid` — veja
  `partitions.csv`), completamente separada da partição `nvs` que a
  aplicação usa (memória de brilho do `hv9910.c`, e qualquer outra
  configuração futura). O comando `FACTORY_RESET` do canal de admin apaga
  só a `nvs` — a `idnvs` nunca é tocada, então serial/chave/época sempre
  sobrevivem a um factory reset.
- Quem souber o segredo mestre consegue recuperar a chave de qualquer
  unidade só a partir do MAC e da época atual (opção "Derive a key from
  MAC+epoch" do `tools/lumtool.py`) — sem precisar de nenhum
  registro/banco de dados central.

### O que é a `epoch` (época), e por que ela existe

`epoch` é basicamente **um número de versão da chave**. Toda unidade
nasce com `epoch=0`. Ele existe por três motivos, todos ligados:

1. **A época entra na fórmula da chave.** Olhando a fórmula acima, `epoch`
   é um dos ingredientes do HKDF (dentro do `info`). Isso significa que
   trocar a época — mesmo mantendo o mesmo segredo mestre e o mesmo MAC —
   produz uma chave **completamente diferente e imprevisível a partir da
   anterior**. É assim que a rotação de chave (`ROTATE_KEY`) funciona: não
   existe "gerar uma chave aleatória nova e guardar em algum lugar" — a
   chave nova é simplesmente a mesma fórmula de sempre, com `epoch`
   incrementado em 1. Não precisa inventar nem armazenar nada extra fora
   da fórmula.
2. **A época é a prova de "qual chave está em vigor agora".** Todo pacote
   autenticado do canal de admin carrega a época que quem está mandando
   *acredita* ser a atual. O dispositivo só aceita o pacote se essa época
   bater com a que ele tem gravada como ativa. Isso é o que impede uma
   chave antiga (de antes de uma rotação) de continuar funcionando depois
   que a chave foi trocada — sem a época, o dispositivo não teria como
   saber se um HMAC válido foi calculado com a chave certa ou com uma
   chave antiga que, por coincidência de fórmula, ainda "parece" válida.
   Pense nela como um rótulo obrigatório: "isto foi assinado com a chave
   versão 3", e o dispositivo só aceita se ele também estiver na versão 3.
3. **A época permite recuperação sem precisar guardar a chave em lugar
   nenhum.** Como a chave é sempre recalculável a partir de
   `(segredo mestre, MAC, epoch)`, você não precisa de um banco de dados
   com "a chave desta unidade é X". Só precisa saber o MAC (já está no
   próprio serial) e a época atual. E se você não souber a época atual —
   por exemplo, esqueceu que já rodou um `rotate-key` nessa unidade há
   meses — o comando `DISCOVER` (o único que não exige autenticação)
   sempre devolve a época atual do dispositivo. É por isso que
   `tools/lumtool.py` sempre manda um `DISCOVER` antes de qualquer outro
   comando: ele usa a época que o próprio dispositivo acabou de informar
   pra derivar a chave certa na hora, e nunca depende de um valor que
   você anotou em algum lugar e pode estar desatualizado.

Resumindo com uma analogia: é como se cada unidade tivesse uma "senha
versão N" — trocar a senha sempre cria automaticamente a versão N+1 (pela
mesma fórmula), o dispositivo só aceita comandos assinados com a versão
que ele mesmo está usando agora, e "qual é a versão atual" é uma
informação pública (o `DISCOVER` conta pra qualquer um), só a senha em si
que é secreta.

**Sem criptografia de flash ou de NVS neste produto.** Essa é uma
escolha deliberada, não um descuido: acesso físico a uma unidade
(dessoldar a flash, ou um ataque de leitura via debug) expõe a chave de
admin **daquela unidade** — mas nunca o segredo mestre (que nunca toca o
dispositivo) e nunca a chave de nenhuma outra unidade (cada chave está
amarrada ao seu próprio MAC via o salt do HKDF). Se uma revisão futura
precisar elevar a barra contra acesso físico a uma unidade isolada,
criptografia de flash é o próximo passo natural, mas não está
implementada aqui.

### Provisionar sem debugger: o comando `CLAIM`

A etapa de produção que grava a chave de admin real (derivada do segredo
mestre) não precisa de conexão serial — pode acontecer inteiramente pela
rede, logo depois do primeiro boot com o firmware de fábrica, através do
comando `CLAIM` (`components/poe_luminaire/admin_channel.c`).

`CLAIM` é **deliberadamente não autenticado** — o mesmo nível de
`DISCOVER`, sem HMAC, sem nonce, sem envelope cifrado nenhum. Isso não é
uma brecha: numa unidade sem identidade ainda, não existe **nenhuma
chave** contra a qual autenticar alguma coisa — qualquer esquema de
"chave temporária de bootstrap" só empurraria o problema pra "como
proteger a chave temporária", sem ganhar segurança real (um valor fixo
commitado no repositório é, por definição, público). A proteção de
verdade é outra: o dispositivo só aceita `CLAIM` **enquanto
`devid_is_provisioned() == false`** (`devid_claim()` em
`components/poe_luminaire/devid.c`). O fluxo inteiro é uma única troca:

```
Cliente -> Dispositivo : CLAIM (payload=chave_real[32] || epoch=0[4], sem autenticação)
Dispositivo -> Cliente  : CLAIM_RESP (status=OK) -- chave real já gravada na idnvs e ativa
```

Assim que uma unidade recebe um `CLAIM` válido, ela grava a chave real
(a mesma fórmula HKDF de sempre) direto na `idnvs`, e **`CLAIM` para de
ser aceito naquela unidade pra sempre** — ele só serve pra dar a
**primeira** identidade a uma unidade virgem; uma segunda tentativa
(engano, retransmissão, ou uma tentativa genuína de "readotar" uma
unidade já em uso) é sempre recusada antes mesmo de olhar o payload.
`tools/lumtool.py` tem uma opção ("Provision a device") que faz esse
`CLAIM` sozinho, sem etapa nenhuma antes.

**O que protege uma unidade recém-ligada, então?** Só a janela de tempo:
qualquer um que alcance a rede de uma unidade ainda não provisionada
consegue reivindicá-la — não há como distinguir "o operador de produção"
de qualquer outra origem nesse momento, porque não há nada ainda pra
verificar. A mitigação é operacional, não criptográfica: provisionar
logo depois de ligar, numa rede controlada (bancada/staging), antes da
unidade tocar uma rede não confiável — quanto menor a janela "ligada mas
não provisionada" numa rede aberta, menor o risco. Uma vez provisionada,
essa janela se fecha permanentemente para aquela unidade: nenhum `CLAIM`
seguinte é aceito, não importa de onde venha.

## Canal de administração (UDP, autenticado)

`components/poe_luminaire/admin_channel.c` roda numa task FreeRTOS própria (prioridade mais
alta que `cmd_server_task`, sem locks/estado compartilhado com
`poe_negotiator`/`cmd_server`), escutando na porta UDP `ADMIN_UDP_PORT`
(`components/poe_luminaire/poe_luminaire.h`, padrão `5001`). Ele existe para descoberta e
recuperação remota quando a luminária está instalada no teto, sem acesso
físico/serial — `DISCOVER` (sem autenticação), `CLAIM` (também sem
autenticação; só aceito em unidades ainda não provisionadas — ver seção
acima), `CHALLENGE`, `STATUS`, `IDENTIFY` (pisca a carga de LED de
verdade — recusa se `poe_negotiator_is_ready()` for falso, mesma regra
que `ON`/`DIM` já seguem), `REBOOT`, `FACTORY_RESET`, e um
`ROTATE_KEY`/`ROTATE_CONFIRM` em duas fases que não consegue tijolar uma
unidade (uma rotação não confirmada simplesmente expira e a chave antiga
continua funcionando).

Como o servidor de comandos TCP, este canal depende da pilha de rede
estar de pé, que continua condicionada à condição "pronto" de
PoE/AUX/VBUS — então ele cobre *a aplicação travar depois de energizada*,
não *a luminária nunca ter recebido energia suficiente*.

### Formato do pacote

Todo pacote (pedido ou resposta) tem exatamente este formato — campos
big-endian, tamanho fixo (sem parsing arriscado de comprimento
variável):

| Offset | Tamanho | Campo         | Descrição |
|-------:|-----:|---------------|-------------|
| 0      | 4    | `magic`       | `0x4C554D31` ("LUM1") |
| 4      | 1    | `version`     | `1` |
| 5      | 1    | `type`        | ver tabela de tipos |
| 6      | 24   | `serial`      | ASCII, preenchido com zeros; ignorado em `DISCOVER`/`DISCOVER_RESP` |
| 30     | 4    | `epoch`       | época que quem manda acredita ser a atual |
| 34     | 16   | `nonce`       | vem do `CHALLENGE`; zero se o comando não exige um |
| 50     | 2    | `payload_len` | tamanho do payload que segue, em bytes |
| 52     | N    | `payload`     | específico de cada tipo (ver abaixo) |
| 52+N   | 32   | `hmac`        | HMAC-SHA256 sobre os bytes `[0, 52+N)`; zero (não verificado) em `DISCOVER`/`DISCOVER_RESP` |

Tamanho total do pacote = `52 + N + 32`. Sempre bem abaixo do MTU de
Ethernet — sem fragmentação IP.

### Tipos de pacote

O bit `0x80` marca "isto é uma resposta" (`REQ | 0x80 = RESP correspondente`).

| Valor | Nome | Autenticado | Precisa de nonce |
|---|---|---|---|
| `0x01` | `DISCOVER` | Não | — |
| `0x81` | `DISCOVER_RESP` | Não | — |
| `0x02` | `CHALLENGE` | Sim | — |
| `0x82` | `CHALLENGE_RESP` | Sim | — (devolve um nonce novo no campo `nonce` da resposta) |
| `0x03` | `STATUS` | Sim | — |
| `0x83` | `STATUS_RESP` | Sim | — |
| `0x04` | `IDENTIFY` | Sim | — |
| `0x84` | `IDENTIFY_RESP` | Sim | — |
| `0x05` | `REBOOT` | Sim | Sim (pool do `CHALLENGE`) |
| `0x85` | `REBOOT_RESP` | Sim | — |
| `0x06` | `FACTORY_RESET` | Sim | Sim (pool do `CHALLENGE`) |
| `0x86` | `FACTORY_RESET_RESP` | Sim | — |
| `0x07` | `ROTATE_KEY` | Sim (chave atual) | Sim (pool do `CHALLENGE`; também serve de IV do AES-GCM) |
| `0x87` | `ROTATE_KEY_RESP` | Sim (chave atual) | — |
| `0x08` | `ROTATE_CONFIRM` | Sim (chave **staged**, i.e. a nova) | Sim (**o mesmo nonce** do `ROTATE_KEY` que originou o staging — não um `CHALLENGE` novo) |
| `0x88` | `ROTATE_CONFIRM_RESP` | Sim (chave já ativa, pós-commit) | — |
| `0x09` | `CLAIM` | Não (só aceito se ainda não provisionado — ver seção acima) | Não |
| `0x89` | `CLAIM_RESP` | Não | — |
| `0xFF` | `ERR_RESP` | Sim | — |

### Validação (lado do dispositivo), nesta ordem, descartando em silêncio na primeira falha

Exceto `DISCOVER` e `CLAIM`, que pulam todas as etapas de autenticação
abaixo — `CLAIM` só passa pela checagem própria de
`devid_is_provisioned()` dentro do seu handler (ver seção acima), nada
disso se aplica a ele:

1. `magic`/`version` corretos.
2. `payload_len` consistente com o tamanho do datagrama recebido.
3. `serial` bate com o do dispositivo.
4. `epoch` bate com a época relevante (ativa, ou a staged pra
   `ROTATE_CONFIRM`).
5. `hmac` bate (`psa_mac_verify`, tempo constante) — com a chave ativa,
   ou a chave staged pra `ROTATE_CONFIRM`.
6. Se o comando exige nonce: bate com um pendente, não expirou, e é
   consumido (uso único).

Qualquer falha nos passos 1-6: o pacote é descartado, **sem nenhuma
resposta** (nem `ERR_RESP`) — um atacante de fora não consegue
diferenciar "chave errada" de "serial errado" de "nonce errado"
observando o tráfego de resposta. `ERR_RESP` só é usado quando o pacote
passa por toda a validação acima mas o comando em si é inválido (ex. tipo
desconhecido).

Limitação de taxa: no máximo 20 pacotes por segundo por IP de origem
(tabela fixa, sem alocação — ver `RATE_LIMIT_*` em `admin_channel.c`);
acima disso, também descartado em silêncio.

### Payloads por tipo

**`DISCOVER_RESP` (41 bytes, sem autenticação)**

| Offset | Tamanho | Campo | Descrição |
|---|---|---|---|
| 0 | 16 | `model` | ASCII preenchido com zeros, ex. `"LUM1"` |
| 16 | 4 | `epoch` | época atual |
| 20 | 16 | `fw_version` | ASCII preenchido com zeros (`esp_app_get_description()->version`) |
| 36 | 4 | `ip` | IPv4 atual, 4 bytes crus |
| 40 | 1 | `provisioned` | `0`/`1` |

Enviado com um atraso aleatório de 0-49ms (evita colisão quando várias
unidades respondem ao mesmo broadcast).

**`STATUS_RESP` (21 bytes)**

| Offset | Tamanho | Campo |
|---|---|---|
| 0 | 4 | `uptime_s` |
| 4 | 1 | `reset_reason` (`esp_reset_reason_t`) |
| 5 | 1 | `poe_ready` (0/1) |
| 6 | 1 | `poe_source` (`poe_source_t`: 0=none, 1=type1, 2=type2, 3=aux) |
| 7 | 1 | `driver_on` (0/1) |
| 8 | 1 | `dim` (0-100) |
| 9 | 4 | `vbus_mv` |
| 13 | 4 | `led_voltage_mv` (bits de um `int32_t`) |
| 17 | 4 | `eth_ip` |

**`IDENTIFY_RESP` / `REBOOT_RESP` / `FACTORY_RESET_RESP` / `ROTATE_KEY_RESP` / `ROTATE_CONFIRM_RESP` / `CLAIM_RESP` / `ERR_RESP` (1 byte)**

Um único byte de status (`admin_status_t` em `components/poe_luminaire/admin_protocol.h`):
`0` = OK, `1` = argumento inválido, `2` = não pronto (ex. `IDENTIFY`
recusado porque PoE/AUX/VBUS não está confirmado — mesma regra que
`ON`/`DIM` já seguem), `3` = não provisionado, `4` = nenhuma rotação de
chave pendente, `5` = erro interno.

**`ROTATE_KEY` (payload de 52 bytes, pedido)**

AES-256-GCM: `encrypt(new_key[32] || new_epoch[4]) + tag[16]`.

- Chave da operação: a chave **atual** do dispositivo.
- Nonce do GCM: os mesmos 16 bytes do campo `nonce` do cabeçalho do
  pacote (obtidos antes via `CHALLENGE`) — nunca reaproveita nonce de
  outro pacote.
- AAD (dado autenticado mas não cifrado): os 52 bytes do próprio
  cabeçalho do pacote — amarra a chave nova a este serial/época/nonce
  específicos.

Ao decifrar com sucesso, o dispositivo **só guarda a chave nova em RAM**
(staged) — nada é gravado, a chave ativa não muda. Ela só entra em vigor
com um `ROTATE_CONFIRM` correspondente, em até 30s; senão se descarta
sozinha e a chave atual continua funcionando.

**`CLAIM` (payload de 36 bytes, pedido, sem autenticação)**

Sem envelope nenhum — payload em texto claro: `new_key[32] ||
new_epoch[4]`. Só aceito enquanto `devid_is_provisioned() == false`
(checado antes de qualquer outra coisa, no handler do comando). Ao
receber, o dispositivo grava a chave **direto** na `idnvs` e já ativa —
sem staging/confirmação: não existe uma identidade anterior pra proteger
contra perda, então uma falha simplesmente deixa a unidade não
provisionada, segura pra tentar de novo. Uma segunda tentativa de `CLAIM`
numa unidade já provisionada é sempre recusada, sem sequer olhar o
payload.

### Exemplo completo de troca (rotação de chave)

```
Cliente -> Dispositivo : CHALLENGE (autenticado com a chave atual, sem nonce)
Dispositivo -> Cliente  : CHALLENGE_RESP (nonce N1 no cabeçalho)

Cliente -> Dispositivo : ROTATE_KEY (nonce=N1, payload=AES-GCM(chave_nova||epoch+1) sob a chave atual)
Dispositivo -> Cliente  : ROTATE_KEY_RESP (status=OK) -- chave nova só staged, ainda não ativa

Cliente -> Dispositivo : ROTATE_CONFIRM (nonce=N1 de novo, autenticado com a chave NOVA)
Dispositivo -> Cliente  : ROTATE_CONFIRM_RESP (status=OK, já autenticado com a chave nova) -- commit feito

# Se o ROTATE_CONFIRM nunca chegar: em 30s o staging expira sozinho e a
# chave ativa nunca muda.
```

Uma troca simples, só leitura (sem rotação):

```
Cliente -> Dispositivo : DISCOVER (broadcast, sem autenticação)
Dispositivo -> Cliente  : DISCOVER_RESP (serial, modelo, epoch, versão, IP)

Cliente -> Dispositivo : CHALLENGE (autenticado com a chave derivada de MAC+epoch)
Dispositivo -> Cliente  : CHALLENGE_RESP (nonce N1)

Cliente -> Dispositivo : STATUS (nonce=N1 não é exigido aqui, mas pode reenviar 0)
Dispositivo -> Cliente  : STATUS_RESP (uptime, motivo do reset, estado de PoE/driver)
```

### Ferramenta de host

Tudo num único arquivo autocontido, sem dependência entre módulos:
`tools/lumtool.py`. Rode sem nenhum argumento — é um menu interativo, sem
flags de linha de comando pra decorar:

```
cd tools
python lumtool.py
```

```
=== LUM1 luminaire admin tool ===

  1) Discover devices on the network
  2) Provision a device (CLAIM, over the network)
  3) Derive a key from MAC+epoch (recovery, no device needed)
  4) Status
  5) Identify (blink)
  6) Reboot
  7) Factory reset
  8) Rotate admin key
  0) Quit
```

- **Discover devices** — varre a rede por broadcast, sem autenticação.
- **Provision a device (CLAIM)** — o único caminho de provisionamento que
  existe nesta ferramenta, inteiramente pela rede: descobre a unidade
  escolhida, confirma que ainda não foi provisionada, deriva a chave real
  de época 0, e manda um `CLAIM` sem autenticação nenhuma (ver
  "Provisionar sem debugger" acima). A unidade grava a própria identidade
  sozinha; a ferramenta acrescenta uma linha a um CSV de produção (serial,
  MAC, modelo, época, versão do firmware, data/hora, operador — **nunca a
  chave**). Não existe caminho de provisionamento por `esptool`/serial
  nesta ferramenta — ela nunca toca a flash diretamente.
- **Status/Identify/Reboot/Factory reset/Rotate admin key** — rodam um
  `discover` primeiro e deixam escolher a unidade numa lista (ou digitar
  o IP direto), sempre derivando a chave a partir do MAC+época que a
  própria unidade acabou de reportar — nunca fica desatualizado depois de
  um `rotate-key`. `Factory reset` exige digitar o serial exato pra
  confirmar. `Rotate admin key` precisa do pacote `cryptography` (`pip
  install cryptography`) para o envelope AES-256-GCM — a única
  dependência fora da biblioteca padrão, e só usada ali, já que
  reimplementar AES em Python puro não é algo pra fazer com
  responsabilidade. `Provision a device` não precisa dela — `CLAIM` não
  tem envelope nenhum, é payload em texto claro.

**Segredo mestre**: por padrão lido de `tools/secret.txt` (gitignored,
nunca commitado). As opções que o usam (*Provision a device*, *Derive a
key*) sempre oferecem digitar o valor na hora (entrada oculta, não salva
em lugar nenhum) em vez de usar o arquivo — útil pra uma gravação avulsa
sem deixar o segredo no disco. Nunca aceito por argumento de linha de
comando, pra não sobrar no histórico do shell.

Os testes (`unittest` da biblioteca padrão — vetores de HKDF do RFC 5869,
um vetor de HMAC, round-trips de serialização do pacote) ficam dentro do
próprio `lumtool.py`, no final do arquivo — é o único `.py` da pasta, de
propósito. Rodam com `python -m unittest lumtool -v` dentro de `tools/`;
como `-m unittest` importa o arquivo pelo nome do módulo (`lumtool`, não
`__main__`), o menu interativo nunca dispara nesse caminho, e vice-versa
(`python lumtool.py` nunca roda os testes).

## Configuração e build

Toda a configuração da aplicação vive em
[components/poe_luminaire/poe_luminaire.h](components/poe_luminaire/poe_luminaire.h) (pinos, porta TCP, polaridades do
HV9910, endereço do PHY, razão do divisor de tensão) — não há nada pra
ajustar em `idf.py menuconfig`. As entradas em
[sdkconfig.defaults](sdkconfig.defaults) são chaves do próprio ESP-IDF:
elas habilitam a compilação do driver EMAC interno, reduzem a
verbosidade padrão do log do console, e declaram o tamanho real da
flash/tabela de partições (`CONFIG_ETH_ENABLED`,
`CONFIG_ETH_USE_ESP32_EMAC`, `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
`CONFIG_LOG_MAXIMUM_LEVEL_INFO`, `CONFIG_BOOTLOADER_LOG_LEVEL_WARN`,
`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `CONFIG_PARTITION_TABLE_CUSTOM`) — não
são parâmetros de hardware do projeto.

### Tamanho de flash / tabela de partições

Esta placa tem um chip de flash de 4MB.
[partitions.csv](partitions.csv), na raiz do projeto, declara uma tabela
de partições dimensionada pra isso (mesmo layout de `nvs`/`phy_init` da
tabela padrão de app único do ESP-IDF, só com uma partição `factory`
maior, mais a partição `idnvs` de identidade — sem partições OTA, este é
um firmware de imagem única).

```
idf.py set-target esp32
idf.py -p PORTA flash monitor
```

Na primeira vez, o gerenciador de componentes do IDF vai baixar o driver
do PHY IP101G (`espressif/ip101`, veja
[components/poe_luminaire/idf_component.yml](components/poe_luminaire/idf_component.yml)) — é necessário acesso à
internet nesse primeiro build.

### Estrutura de componentes

`main/` contém só `poe_luminaire_main.c` (o `app_main()`) — tudo o resto
(driver do HV9910, negociação PoE, sensor de tensão, Ethernet, servidor
de comandos, identidade do dispositivo, canal de administração, e
`poe_luminaire.h` com toda a configuração da placa) mora em
`components/poe_luminaire/`, como um componente ESP-IDF próprio,
requerido por `main/CMakeLists.txt`. O ESP-IDF descobre a pasta
`components/` na raiz do projeto automaticamente — não precisa de
nenhuma configuração extra (`EXTRA_COMPONENT_DIRS` etc.) pra isso
funcionar.

## Verbosidade do log no console

Por padrão, o ESP-IDF imprime bastante ruído de inicialização (banner do
bootloader, dump da tabela de partições, carregamento de segmentos de
imagem, sondagem de heap/flash, etc) antes mesmo do `app_main()` rodar.
Este projeto silencia tudo isso via `sdkconfig.defaults` (nível de log
padrão da aplicação elevado pra `WARN`, nível de log do bootloader
elevado pra `WARN`) e depois reabilita explicitamente o log `INFO` pras
suas próprias oito tags de módulo bem no início do `app_main()` — veja
`quiet_boot_noise()` em
[main/poe_luminaire_main.c](main/poe_luminaire_main.c). Avisos/erros
internos do IDF ainda são impressos; só a conversa rotineira de nível
INFO é suprimida.

## Log de eventos de PoE/AUX/VBUS

Toda vez que o sinal (com debounce) de CDB, T2P, ou "VBUS acima do
limiar" muda de estado, `poe_negotiator` registra isso imediatamente e de
forma independente, não importa se isso vira ou não o veredito geral de
pronto/não-pronto:

```
I (...) POE_NEG: EVENT: CDB asserted — real PoE negotiated (inrush done)
W (...) POE_NEG: EVENT: T2P dropped — no AUX/Type-2 confirmation anymore
W (...) POE_NEG: EVENT: VBUS too low — 18300mV < 40000mV threshold
```

Isso importa porque as três condições são independentes
(`ready = (PoE OK ou AUX presente) e VBUS ok`) — ex. o PoE real pode cair
enquanto a fonte AUX de bancada ainda está segurando o sistema, ou
CDB/T2P podem estar OK enquanto o próprio VBUS está caindo abaixo do
limiar, e ambos os casos merecem sua própria linha de log mesmo quando
mais nada muda. Além disso, o veredito combinado em si é logado a cada
transição (`READY: ...` / `LOW POWER MODE: ...`, e o segundo diz
explicitamente se a causa foi "nenhuma fonte digital" ou "fonte digital
OK mas VBUS baixo demais"), mais uma linha de lembrete que imprime a cada
5s enquanto travado em modo de baixo consumo, pra o console nunca ficar
quieto por muito tempo durante o bring-up.

## Estrutura do código

`main/` tem só o entry point; todo o resto do firmware é o componente
`components/poe_luminaire/` (veja "Estrutura de componentes" acima):

| Arquivo                                             | Responsabilidade                                                  |
| ---------------------------------------------------- | ---------------------------------------------------------------|
| `main/poe_luminaire_main.c`                          | `app_main()`: orquestra a sequência de boot — tag de log `MAIN` |
| `components/poe_luminaire/poe_luminaire.h`      | Mapa de pinos, suposições de hardware, e todos os valores de configuração da aplicação |
| `components/poe_luminaire/hv9910.[ch]`               | Controle do driver de LED (SHUTDOWN + PWM de dimerização), persistência de brilho/estado em NVS — tag de log `HV9910` |
| `components/poe_luminaire/poe_negotiator.[ch]`       | Monitoramento de CDB/T2P (PoE/AUX) e VBUS do TPS2378, watchdog de segurança — tag de log `POE_NEG` |
| `components/poe_luminaire/voltage_sense.[ch]`        | Leitura de tensão VBUS/VLED (ADC_VLED_P/ADC_VLED_N) — tag de log `VOLT_SENSE` |
| `components/poe_luminaire/eth_init.[ch]`             | Bring-up do PHY IP101G / Ethernet / DHCP — tag de log `ETH_INIT` |
| `components/poe_luminaire/cmd_server.[ch]`           | Servidor de comandos TCP em texto — tag de log `CMD_SRV` |
| `components/poe_luminaire/devid.[ch]`                | Identidade do dispositivo: serial, chave/época, claim remoto, staging de rotação de chave — tag de log `DEVID` |
| `components/poe_luminaire/admin_protocol.h`          | Formato de pacote do canal de admin (struct, enums de tipo/status) — sem lógica |
| `components/poe_luminaire/admin_channel.[ch]`        | Canal UDP de administração autenticado (task própria) — tag de log `ADMIN_CH` |
| `components/poe_luminaire/idf_component.yml`         | Dependência gerenciada do driver de PHY IP101G |
| `tools/lumtool.py`                                   | Ferramenta de host única e autocontida: menu interativo com provisionamento e administração remota |
