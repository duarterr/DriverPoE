| Supported Targets | ESP32 |
| ----------------- | ----- |

# DriverPoE — firmware mínimo

Firmware mínimo para uma luminária alimentada por PoE, construído sobre:

- **ESP32** (EMAC interno)
- **IP101G** — PHY Ethernet via RMII
- **TPS2378** — negociador/interface PoE (IEEE 802.3at, PD)
- **HV9910** — driver de LED (buck, dimerização por PWM filtrado no LDIM)

## Comportamento

1. No boot, antes de qualquer outra inicialização, o driver de LED HV9910
   é forçado ao estado desabilitado (`hv9910_init()`).
2. O ADC (`voltage_sense_init()`) sobe em seguida, para que o VBUS já
   esteja legível assim que o gate de PoE/AUX começar a avaliá-lo.
3. O firmware passa a monitorar os pinos **CDB** e **T2P** do TPS2378,
   cruzados com a tensão medida do barramento DC (**VBUS**, lida via
   `ADC_CH_VLED_P`). O sistema é considerado "pronto" (seguro para operar)
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
     de `VBUS_MIN_MV` (padrão 40V, `main/poe_luminaire_main.h`). CDB/T2P são só
     sinais digitais de handshake; VBUS é a checagem final de que o
     barramento está de fato saudável antes de confiar o driver de LED
     pra rodar. Cada leitura de VBUS usada nessa decisão já é a mediana de
     3 amostras rápidas do ADC (rejeita uma amostra isolada com ruído,
     antes mesmo do debounce), e o limiar tem histerese: `VBUS_MIN_MV`
     pra **entrar** em "ok", um limiar `VBUS_HYSTERESIS_MV` mais baixo pra
     **sair** — sem isso, uma leitura em cima da hora (~40V) faria o
     veredito oscilar a cada ruído pequeno do barramento. CDB, T2P e o
     veredito de VBUS (já com a histerese acima) continuam todos passando
     por debounce temporal (5 leituras estáveis) antes de contar como
     confirmados — ver `components/tps2378/tps2378.c`.

   Enquanto **qualquer** um dos requisitos falhar ("modo de baixo
   consumo"), o driver de LED fica travado — inclusive se a condição
   combinada cair de novo depois de já ter estado OK (perda de energia,
   AUX removido, VBUS caindo, sobrecarga térmica, renegociação), o
   firmware corta o driver imediatamente (`hv9910_emergency_disable()` —
   ver "Persistência de brilho" abaixo pra por que essa chamada é
   diferente de um `OFF` comum). Veja
   [components/tps2378/tps2378.h](components/tps2378/tps2378.h) para a lógica completa e
   o raciocínio, e "LEDs indicadores" abaixo pro que aparece nos dois LEDs
   da placa enquanto isso.
4. Assim que o sistema fica pronto, o firmware:
   - inicializa o PHY IP101G e sobe a pilha Ethernet (cliente DHCP por
     padrão, via `esp_netif`/lwIP) — a criação do MAC/PHY/driver/netif é
     tentada de novo, com backoff, até 5 vezes se falhar (sem reiniciar o
     dispositivo pra tentar de novo — ver `components/eth_init/eth_init.c`);
   - só se essa tentativa realmente funcionar, inicia o canal de
     administração UDP autenticado (porta configurável, padrão 5001) —
     ver seção própria abaixo. Se todas as tentativas de subir a Ethernet
     falharem, o canal de administração **não** é iniciado (não faria
     sentido sem pilha de rede) e o dispositivo continua rodando
     normalmente todo o resto (PoE, driver, LEDs indicadores) — só fica
     inacessível pela rede até o próximo boot.
5. A partir daí, comandos autenticados recebidos pela rede podem
   ligar/desligar o driver, ajustar o brilho e ler tensões/estado.

Veja [main/poe_luminaire_main.h](main/poe_luminaire_main.h) para o mapa de pinos completo
e para **todo parâmetro de configuração da aplicação** (porta UDP do canal
de administração, polaridades do HV9910, frequência de PWM, endereço do
PHY, razão do divisor de tensão) — tudo como `#define`s, deliberadamente
sem nada no Kconfig/menuconfig. Pra mudar qualquer um desses valores,
edite `poe_luminaire_main.h` e recompile.

## LEDs indicadores

Duas luzes indicadoras na placa, cada uma resolvendo um pisca-pisca
diferente do que aparece na carga de LED de verdade (essa é controlada
pelo HV9910, cuja API o `components/hv9910/hv9910.[ch]` já expõe —
ver "Persistência de brilho" abaixo). São GPIOs de lógica direta e
simples (HIGH = aceso), sem PWM nem inversão nenhuma — ao contrário do
pino DIMMING do HV9910:

| LED | Pino | Significado |
|---|---|---|
| **Azul** | GPIO14 (`PIN_LED_BLUE`) | Energia/tensão. Acende assim que o firmware inicializa esse módulo (não espera a negociação de PoE terminar) e fica sólido assim que o VBUS é confirmado; pisca enquanto isso não acontece — cobre tanto "sem energia nenhuma" quanto "fonte digital (CDB/T2P) confirmada mas o barramento ainda está baixo demais". |
| **Vermelho** | GPIO12 (`PIN_LED_RED`) | Estado do driver HV9910. Aceso enquanto o driver está habilitado (qualquer brilho — um LED indicador simples não mostra nível de dimerização, então "10%" e "100%" aparecem os dois como aceso), apagado enquanto desabilitado. Reservado para o futuro: piscar pra sinalizar falha na fita/string de LED, uma vez que essa detecção exista (**ainda não implementada**). |

Toda essa lógica mora sozinha em
[components/status_leds/status_leds.c](components/status_leds/status_leds.c) — é o único lugar do firmware
que decide o que os LEDs devem mostrar. Nenhum outro módulo (`hv9910`,
`tps2378`, `eth_init`, ...) toca `PIN_LED_BLUE`/`PIN_LED_RED`
diretamente — cada um deles já tem seu próprio trabalho, e "o que os
LEDs indicadores significam" não é responsabilidade de nenhum deles.

`status_leds` não depende de nenhum outro componente em tempo de
compilação (nem inclui `tps2378.h`/`hv9910.h`): quem sabe o que
"energia ok" e "driver ligado" significam é o `main`, que passa isso
como ponteiros de função (`power_ok_fn`/`driver_on_fn` em
`status_leds_config_t`) na hora do `status_leds_init()`. É essa
inversão que permite `status_leds_init()` ser a primeiríssima coisa
que `app_main()` faz — antes até de `hv9910_init()`/`tps2378_init()` —
pra o azul acender assim que o firmware sobe, como indicação de "ESP
ligado". Os ponteiros só precisam ser válidos quando a task de
`status_leds` de fato os chamar (nesse caso `tps2378_vbus_confirmed()`
e `hv9910_is_enabled()`), não no momento em que são passados — e como
os getters desses dois módulos são só leitura de uma variável estática
que já nasce `false`, o comportamento antes dos respectivos `_init()`
rodarem já é o correto (azul piscando, vermelho apagado).

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

## Persistência de brilho e estado ligado/desligado

Dois valores são salvos na NVS (namespace `hv9910`) toda vez que mudam,
e recarregados no boot:

- **Brilho** (chave `dim`) — o último brilho não-zero, atualizado a cada
  `DIM`. Um dispositivo novo, sem nada salvo ainda, usa 100% por padrão.
- **Estado ligado/desligado** (chave `on`) — se o driver foi deixado
  ligado ou desligado da última vez. Um dispositivo novo, sem nada salvo
  ainda, é tratado como desligado (nunca liga sozinho numa unidade
  virgem).

### API, e por que ela é segura de chamar de qualquer task

`components/hv9910/hv9910.c` expõe estas funções:

| Função | Faz |
|---|---|
| `hv9910_enable(ramp_ms, persist)` | Libera o `SHUTDOWN` e sobe até o brilho lembrado, ao longo de `ramp_ms` |
| `hv9910_disable(ramp_ms, persist)` | Desce o brilho até 0 ao longo de `ramp_ms`, só então trava o `SHUTDOWN` |
| `hv9910_emergency_disable()` | Igual a `hv9910_disable(0, false)`, mas fura a fila de comandos — só o corte de segurança do `tps2378` usa essa |
| `hv9910_set_dim(percent, ramp_ms)` | Muda só o brilho, não mexe no `SHUTDOWN` |
| `hv9910_identify()` | Pisca algumas vezes e restaura o estado anterior — usada pelo `IDENTIFY` do canal admin |

`ramp_ms = 0` nas que aceitam esse parâmetro significa instantâneo;
qualquer valor maior que `HV9910_MAX_RAMP_MS` (`poe_luminaire_main.h`, `10000`
= 10s) é cortado nesse teto — existe pra um `ramp_ms` absurdo (um pacote
malformado, um bug futuro do lado do host) nunca virar uma rampa ou uma
espera efetivamente sem fim. O `persist` de `enable`/`disable` controla
só se o novo estado ligado/desligado é gravado na NVS (chave `on`,
namespace `hv9910`) — `true` quando é intenção real do operador (`ON`,
`OFF`, `DIM` ligando/desligando o driver, que deve sobreviver a
reinícios e quedas de energia); `false` para mudanças transitórias que
não devem redefinir "o que o operador quer" — o corte de segurança do
`tps2378` quando a energia cai, o religamento automático quando
ela volta, e o pisca-pisca temporário do `IDENTIFY`.

**Por que o `persist` importa de verdade**: se o corte de segurança por
queda de PoE gravasse "desligado" toda vez que dispara, o religamento
automático nunca funcionaria depois de uma queda de energia real — a
própria queda apagaria a memória de "estava ligado" um instante antes de
precisar dela. Por isso `tps2378.c` chama `hv9910_emergency_disable()`
no corte (instantâneo, não grava, e fura a fila — ver abaixo) e
`hv9910_enable(HV9910_DEFAULT_RAMP_MS, false)` no religamento (rampa
padrão, não grava — só está restaurando um estado que já foi gravado
antes, não criando intenção nova).

**Concorrência**: toda operação física de GPIO/LEDC do HV9910 acontece
numa única task interna a `hv9910.c`, dirigida por uma fila de comandos —
nenhum outro módulo (`tps2378`, `admin_channel`, `status_leds`)
toca o hardware do driver diretamente. As funções acima só empacotam um
comando e devolvem o controle na hora; é isso que torna seguro chamá-las
de qualquer task (a task de monitoramento do `tps2378`, a task do
canal admin, ...) sem duas delas colidirem no mesmo registro do LEDC ou
pisarem no mesmo estado interno. Um comando novo sempre cancela o que a
task estava esperando terminar (um `SHUTDOWN` adiado de um `OFF` com
rampa, ou o próximo passo de um `IDENTIFY` em andamento) antes de agir —
é assim que um `OFF` com rampa seguido de um `ON` não desliga a luminária
de novo mais tarde por causa de uma espera antiga, e que um `ON`/`OFF`/`DIM`
real interrompe um `IDENTIFY` no meio do pisca-pisca em vez de
competir com ele pelo mesmo hardware. `hv9910_emergency_disable()`
existe à parte porque a perda de energia PoE/AUX precisa preemptar
qualquer backlog de comandos (inclusive um `IDENTIFY` de vários segundos)
em vez de esperar a vez — ela entra na FRENTE da fila, não no fim; veja
os comentários em `hv9910.c` (`hv9910_task()`) para os detalhes de
implementação.

`hv9910_enable()` sempre reaplica o brilho lembrado (chave `dim`) antes
de liberar o `SHUTDOWN`, então o driver nunca volta apagado. Um
dispositivo novo, sem nada salvo ainda, usa 100% de brilho por padrão e
começa desligado (nunca liga sozinho numa unidade virgem). Veja
[components/hv9910/hv9910.h](components/hv9910/hv9910.h) para a API completa e o raciocínio de cada
função. Isso usa a partição `nvs` já declarada em
[partitions.csv](partitions.csv).

## Rampas de brilho

Toda rampa é feita pelo próprio hardware do LEDC
(`ledc_set_fade_with_time()` + `ledc_fade_start()`, modo
`LEDC_FADE_NO_WAIT`) — não é um laço de software indo passo a passo,
então não bloqueia a task que chamou nem consome CPU durante a rampa.
`ramp_ms` é sempre em milissegundos, a unidade nativa do periférico —
inclusive nos comandos `ON`/`OFF`/`DIM` do canal de administração (campo
`ramp_ms` de 4 bytes no payload, big-endian), sem conversão nenhuma no
meio do caminho. Veja "Payloads por tipo" mais abaixo para o formato
exato de cada um. O padrão sugerido pelas ferramentas quando o operador
não informa um valor é `HV9910_DEFAULT_RAMP_MS`
(`main/poe_luminaire_main.h`, `250`).

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

A correção é nunca disparar duas rampas seguidas no mesmo canal. Duas
coisas garantem isso hoje: primeiro, toda chamada que efetivamente toca o
hardware do LEDC roda dentro da única task interna de `hv9910.c` (ver
"Concorrência" acima) — não existe mais nenhum outro código em lugar
nenhum do firmware que possa disparar uma rampa concorrente. Segundo,
quando o `DIM` do canal admin precisa ligar o driver a partir de
desligado com um alvo específico, ele ainda chama `hv9910_enable(0, true)`
primeiro — `ramp_ms=0` faz isso passar pelo caminho instantâneo (que
nunca toca o hardware de fade), então não compete com a rampa de verdade
que vem logo em seguida via `hv9910_set_dim(valor, rampa_ms)`. Só uma
rampa é disparada por comando, sempre.

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
(`"DriverPoE-<12 hex do MAC base>"`, ex. `DriverPoE-A4CF12B93D08`) e uma **chave de
administração** de 32 bytes, ambos tratados por `components/devid/devid.h`/`components/devid/devid.c`:

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
comando `CLAIM` (`components/admin_channel/admin_channel.c`).

`CLAIM` é **deliberadamente não autenticado** — o mesmo nível de
`DISCOVER`, sem HMAC, sem nonce, sem envelope cifrado nenhum. Isso não é
uma brecha: numa unidade sem identidade ainda, não existe **nenhuma
chave** contra a qual autenticar alguma coisa — qualquer esquema de
"chave temporária de bootstrap" só empurraria o problema pra "como
proteger a chave temporária", sem ganhar segurança real (um valor fixo
commitado no repositório é, por definição, público). A proteção de
verdade é outra: o dispositivo só aceita `CLAIM` **enquanto
`devid_is_provisioned() == false`** (`devid_claim()` em
`components/devid/devid.c`). O fluxo inteiro é uma única troca:

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

`components/admin_channel/admin_channel.c` roda numa task FreeRTOS
própria (prioridade mais alta que a task de monitoramento do
`tps2378`, sem locks/estado compartilhado com ele), escutando na
porta UDP `ADMIN_UDP_PORT` (`main/poe_luminaire_main.h`,
padrão `5001`). Este é o **único ponto de controle pela rede** que o
firmware expõe — não existe mais um servidor de comandos TCP separado;
tudo, desde ligar/desligar a luminária até recuperação remota, passa por
aqui, autenticado. Comandos: `DISCOVER` (sem autenticação), `CLAIM`
(também sem autenticação; só aceito em unidades ainda não provisionadas —
ver seção acima), `CHALLENGE`, `STATUS`, `ON`/`OFF`/`DIM` (controle do
driver de LED, com rampa — mesma lógica que existia no antigo servidor
TCP, ver "Payloads por tipo" abaixo), `IDENTIFY` (pisca a carga de LED de
verdade — recusa se `tps2378_is_ready()` for falso, mesma regra
que `ON`/`DIM` já seguem), `REBOOT`, `FACTORY_RESET`, e um
`ROTATE_KEY`/`ROTATE_CONFIRM` em duas fases que não consegue tijolar uma
unidade (uma rotação não confirmada simplesmente expira e a chave antiga
continua funcionando).

Este canal depende da pilha de rede estar de pé, que continua
condicionada à condição "pronto" de PoE/AUX/VBUS — então ele cobre *a
aplicação travar depois de energizada*, não *a luminária nunca ter
recebido energia suficiente*.

### Formato do pacote

Todo pacote (pedido ou resposta) tem exatamente este formato — campos
big-endian, tamanho fixo (sem parsing arriscado de comprimento
variável):

| Offset | Tamanho | Campo         | Descrição |
|-------:|-----:|---------------|-------------|
| 0      | 4    | `magic`       | `0x44504F45` ("DPOE") |
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
| `0x0A` | `ON` | Sim | — |
| `0x8A` | `ON_RESP` | Sim | — |
| `0x0B` | `OFF` | Sim | — |
| `0x8B` | `OFF_RESP` | Sim | — |
| `0x0C` | `DIM` | Sim | — |
| `0x8C` | `DIM_RESP` | Sim | — |
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
| 0 | 16 | `model` | ASCII preenchido com zeros, ex. `"DriverPoE"` |
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
| 6 | 1 | `poe_source` (`tps2378_source_t`: 0=none, 1=type1, 2=type2, 3=aux) |
| 7 | 1 | `driver_on` (0/1) |
| 8 | 1 | `dim` (0-100) |
| 9 | 4 | `vbus_mv` |
| 13 | 4 | `led_voltage_mv` (bits de um `int32_t`) |
| 17 | 4 | `eth_ip` |

**`ON` (payload de 4 bytes, pedido)**

| Offset | Tamanho | Campo |
|---|---|---|
| 0 | 4 | `ramp_ms` |

Habilita o HV9910, subindo até o último brilho lembrado ao longo de
`ramp_ms` milissegundos. Recusado com `ERR_NOT_READY` a menos que PoE ou
AUX esteja confirmado (`tps2378_is_ready()`) — mesma regra de
segurança de sempre.

**`OFF` (payload de 4 bytes, pedido)**

| Offset | Tamanho | Campo |
|---|---|---|
| 0 | 4 | `ramp_ms` |

Desce o brilho até apagar ao longo de `ramp_ms` milissegundos, só então
trava o `SHUTDOWN`. Sem gate de PoE/AUX — desligar é sempre permitido.

**`DIM` (payload de 5 bytes, pedido)**

| Offset | Tamanho | Campo |
|---|---|---|
| 0 | 1 | `percent` (0-100) |
| 1 | 4 | `ramp_ms` |

Faz a transição de brilho até `percent` ao longo de `ramp_ms`
milissegundos. `percent=0` também desabilita o driver assim que a rampa
termina (se estava ligado); `percent>0` também habilita o driver se
estava desligado (mesmo gate de PoE/AUX do `ON` — mas, diferente do `ON`,
o `DIM` nunca recusa: se o gate não deixar ligar agora, o brilho pedido
fica gravado e é aplicado assim que a energia permitir).

**`ON_RESP` / `OFF_RESP` / `DIM_RESP` / `IDENTIFY_RESP` / `REBOOT_RESP` / `FACTORY_RESET_RESP` / `ROTATE_KEY_RESP` / `ROTATE_CONFIRM_RESP` / `CLAIM_RESP` / `ERR_RESP` (1 byte)**

Um único byte de status (`admin_status_t` em `components/admin_channel/admin_protocol.h`):
`0` = OK, `1` = argumento inválido, `2` = não pronto (ex. `ON`/`IDENTIFY`
recusados porque PoE/AUX/VBUS não está confirmado), `3` = não
provisionado, `4` = nenhuma rotação de chave pendente, `5` = erro
interno.

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

Ligando a 80% de brilho ao longo de 500ms (`ON`/`OFF`/`DIM` seguem o
mesmo padrão — sem `CHALLENGE`/nonce, igual `STATUS`/`IDENTIFY`):

```
Cliente -> Dispositivo : DIM (autenticado com a chave ativa, payload=percent=80 || ramp_ms=500)
Dispositivo -> Cliente  : DIM_RESP (status=OK)
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
=== DriverPoE admin tool ===

(no scan yet)
  [S] Scan the network
  [M] Enter a device IP manually
  [K] Derive a key from MAC+epoch (recovery, no device needed)
  [Q] Quit
```

O menu é sempre "primeiro escaneia, depois escolhe a unidade" (estilo
`diskpart`): `[S]` varre a rede por broadcast (sem autenticação) e lista
cada unidade encontrada, numerada, com serial/IP/época/firmware e se está
provisionada; digitar o número entra no menu daquela unidade específica.
`[M]` faz o mesmo pra uma unidade que não respondeu ao broadcast (outra
sub-rede, por exemplo), a partir de um IP digitado direto. `[K]` deriva
uma chave a partir de MAC+época sem precisar de nenhuma unidade
respondendo (recuperação).

Uma unidade ainda não provisionada só mostra `Provision (CLAIM)`. Uma já
provisionada mostra os comandos agrupados em submenus:

```
=== DriverPoE-A4CF12B93D08  ip=192.168.1.50 ===
  Status: provisioned
  1) Info & control  (status / identify / on / off / dim)
  2) Administration  (reboot / factory reset / rotate key)
  0) Back to device list
```

- **Provision (CLAIM)** — o único caminho de provisionamento que existe
  nesta ferramenta, inteiramente pela rede: confirma que a unidade ainda
  não foi provisionada, deriva a chave real de época 0, e manda um
  `CLAIM` sem autenticação nenhuma (ver "Provisionar sem debugger"
  acima). A unidade grava a própria identidade sozinha; a ferramenta
  acrescenta uma linha a um CSV de produção (serial, MAC, modelo, época,
  versão do firmware, data/hora, operador — **nunca a chave**). Não
  existe caminho de provisionamento por `esptool`/serial nesta
  ferramenta — ela nunca toca a flash diretamente.
- **Info & control → On/Off/Dim** — controlam o driver de LED pela rede
  (o único caminho que existe agora — não há mais servidor TCP separado).
  Pedem a rampa em milissegundos (Enter usa o padrão da placa,
  `DEFAULT_RAMP_MS` no próprio `lumtool.py`, que precisa ficar igual a
  `HV9910_DEFAULT_RAMP_MS` em `poe_luminaire_main.h`); `Dim` pede também o
  brilho alvo (0-100).
- **Info & control → Status/Identify** e **Administration →
  Reboot/Factory reset/Rotate admin key** — sempre derivam a chave a
  partir do MAC+época que a própria unidade acabou de reportar no scan —
  nunca fica desatualizado depois de um `rotate-key`. `Factory reset`
  exige digitar o serial exato pra confirmar. `Rotate admin key` precisa
  do pacote `cryptography` (`pip install cryptography`) para o envelope
  AES-256-GCM — a única dependência fora da biblioteca padrão, e só
  usada ali, já que reimplementar AES em Python puro não é algo pra
  fazer com responsabilidade. `Provision`/`On`/`Off`/`Dim` não precisam
  dela — nenhum dos três tem envelope cifrado, só HMAC (ou, no caso do
  `CLAIM`, nem isso).

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

## Strapping pins e o conector extra

Quatro sinais desta placa caem em pinos de strapping do ESP32 clássico —
que não têm estado elétrico garantido no instante do reset (antes de
qualquer firmware rodar), porque quem os define hoje é só um resistor de
LED, um pull-up habilitado tarde demais pelo firmware, ou o que o PHY
Ethernet estiver fazendo no próprio power-up dele. Um quinto (o clock RMII
externo) é strap também, mas por uma limitação de silício não dá pra
apontar pra outro pino sem inverter quem gera esse clock. Ver a análise
completa de cada um (o que o strap decide, e como o sinal específico desta
placa interfere) no histórico do projeto — aqui vai só o plano de troca.

Esta placa tem um conector extra, ainda sem função, com seis pinos livres
de qualquer strap e qualquer restrição de fabricação de módulo:
`PIN_EXTRA1`..`PIN_EXTRA6` em
[main/poe_luminaire_main.h](main/poe_luminaire_main.h) (GPIO36, GPIO39,
GPIO13, GPIO4, GPIO16, GPIO17). Plano de troca pra próxima revisão de PCB:

| Sinal atual | Pino atual | Vira | Pino novo | Observação |
|---|---|---|---|---|
| `PIN_LED_BLUE` | GPIO12 (MTDI — strap de **tensão do flash**) | `PIN_EXTRA3` | GPIO13 | O mais urgente de tirar: hoje o resistor do LED azul, sozinho, provavelmente já força `VDD_SDIO=1.8V` num flash de 3.3V. |
| `PIN_POE_T2P` | GPIO2 (strap **crítico** de boot mode) | `PIN_EXTRA2` | GPIO39 | Pino de entrada apenas — T2P nunca precisa ser saída. **Exige pull-up externo** (GPIO39 não tem pull interno), que essa placa não tem hoje mesmo no pino atual. |
| `PIN_POE_CDB` | GPIO15 (MTDO — strap secundário + JTAG) | `PIN_EXTRA1` | GPIO36 | Mesma lógica do T2P: entrada só, **exige pull-up externo**. |
| `PIN_ETH_PHY_RESET` | GPIO5 (strap secundário) | `PIN_EXTRA4` | GPIO4 | Sem função especial nenhuma — troca direta, sem pré-requisito. |
| `PIN_ETH_REF_CLK` | GPIO0 (strap **crítico** de boot mode) | *(condicional)* | GPIO16 (`PIN_EXTRA5`) | Só é possível **invertendo quem gera o clock**: trocar `EMAC_CLK_EXT_IN` por `EMAC_CLK_OUT` em `eth_init.c` (o ESP32 passa a gerar os 50MHz, não o IP101G). **Exige confirmar no datasheet do IP101G que ele aceita operar como clock slave** e popular/remover componentes no lado do PHY de acordo — não é só reroteamento. Se essa condição não se confirmar, GPIO0 fica onde está; `PIN_EXTRA6` (GPIO17) sobra livre como alternativa de saída de clock (`EMAC_CLK_OUT_180`) caso o layout físico favoreça essa trilha em vez da 16. |

Depois da troca, `PIN_LED_RED` (GPIO14, JTAG MTMS mas não é strap) é o
único sinal que ainda compartilha função com o JTAG — aceitável, já que
não decide nada no boot.

## Configuração e build

Toda a configuração da aplicação vive em
[main/poe_luminaire_main.h](main/poe_luminaire_main.h) (pinos, porta UDP do
canal de administração, polaridades do HV9910, endereço do PHY, razão do
divisor de tensão) — não há nada pra ajustar em `idf.py menuconfig`. As
entradas em
[sdkconfig.defaults](sdkconfig.defaults) são chaves do próprio ESP-IDF:
elas habilitam a compilação do driver EMAC interno, reduzem a
verbosidade padrão do log do console, e declaram o tamanho real da
flash/tabela de partições (`CONFIG_ETH_ENABLED`,
`CONFIG_ETH_USE_ESP32_EMAC`, `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
`CONFIG_LOG_MAXIMUM_LEVEL_INFO`, `CONFIG_BOOTLOADER_LOG_LEVEL_WARN`,
`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `CONFIG_PARTITION_TABLE_CUSTOM`,
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) — não são parâmetros de hardware
do projeto.

### Tamanho de flash / tabela de partições

Esta placa tem um chip de flash de 4MB.
[partitions.csv](partitions.csv), na raiz do projeto, declara uma tabela
de partições dimensionada pra isso: `nvs`/`phy_init` nos mesmos offsets
da tabela padrão do ESP-IDF, `otadata` + duas partições de app (`ota_0`/
`ota_1`, 1MB cada — ver "Atualização OTA" abaixo) em vez de uma única
`factory`, mais a partição `idnvs` de identidade.

```
idf.py set-target esp32
idf.py -p PORTA flash monitor
```

Na primeira vez, o gerenciador de componentes do IDF vai baixar o driver
do PHY IP101G (`espressif/ip101`, veja
[components/eth_init/idf_component.yml](components/eth_init/idf_component.yml)) — é necessário acesso à
internet nesse primeiro build.

## Atualização OTA

A tabela de partições já está pronta pra OTA segura — `otadata` + duas
partições de aplicação (`ota_0`/`ota_1`, 1MB cada) em vez de uma única
`factory` — mas **nenhuma lógica de atualização (a metade que grava uma
imagem nova) existe ainda**: não há chamada a `esp_https_ota`/
`esp_ota_begin`+`write`+`end` em lugar nenhum deste firmware. Isso é
deliberado — o objetivo aqui foi só deixar a estrutura pronta, não
inventar uma API de atualização improvisada. A outra metade do contrato
de rollback — confirmar a imagem que já está rodando — está implementada,
ver abaixo.

- **Por que dois slots de 1MB**: o binário atual (`driverpoe.bin`)
  tem hoje ~450KB — cada slot de 1MB sobra mais de 2x de margem (`idf.py
  build` imprime a % livre de cada partição de app a cada build). Isso
  cabe com folga nos 4MB desta placa junto com `nvs`/`phy_init`/`otadata`/
  `idnvs`, ainda sobrando ~1.9MB de flash sem uso. Se o firmware algum dia
  crescer a ponto de dois slots de 1MB não caberem mais com margem
  adequada nos 4MB, a resposta correta é reavaliar o orçamento de flash
  (reduzir alguma dependência, ou considerar um chip maior) — nunca
  encolher os slots a ponto de perder a margem de segurança de uma
  atualização, e nunca voltar a uma partição `factory` única só pra
  "resolver" o aperto.
- **Por que `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` já está ligado**
  (`sdkconfig.defaults`): com dois slots de OTA, é o bootloader que
  protege contra um update ruim — depois que um mecanismo de atualização
  (ainda não implementado) trocar o slot de boot pra uma imagem nova, o
  bootloader marca essa imagem como "pendente de confirmação" e, se ela
  nunca se confirmar (trava, watchdog, falta de energia logo no primeiro
  boot), volta sozinho pro slot anterior no próximo boot — sem precisar
  de nenhuma lógica extra aqui. A contrapartida — confirmar a imagem que
  conseguiu rodar, cancelando o rollback — **já está implementada**:
  `confirm_app_if_pending_verify()` em `main/poe_luminaire_main.c`, chamada
  logo no início do `app_main()` (depois do `status_leds_init()`, antes de
  qualquer outro módulo), chama `esp_ota_mark_app_valid_cancel_rollback()`
  sempre que a partição rodando ainda está `ESP_OTA_IMG_PENDING_VERIFY`.
  Sem essa chamada, todo boot de uma imagem gerenciada por OTA ficaria
  "pendente" pra sempre e reverteria sozinho a cada reset não confirmado —
  não só no primeiro boot depois de um update real. Não existe nenhuma
  lógica de auto-teste por trás disso ainda (confirma incondicionalmente,
  só por ter chegado até `app_main()`); se um mecanismo de atualização
  real for implementado, vale considerar gating essa confirmação em algum
  critério mais forte (ex.: Ethernet up, PoE negociado) antes de cancelar
  o rollback.
- **O que falta pra OTA funcionar de verdade** (fora do escopo deste
  documento, só pra deixar claro o que "pronto" significa aqui): um
  cliente que baixe a imagem nova (`esp_https_ota` é o caminho padrão do
  ESP-IDF), grave no slot inativo, valide, e só então chame
  `esp_ota_set_boot_partition()` — nada disso existe neste firmware hoje.

### Estrutura de componentes

`main/` contém só dois arquivos: `poe_luminaire_main.c` (o `app_main()`) e
`poe_luminaire_main.h` (todo o mapa de pinos/config da placa). Cada
módulo de hardware é seu **próprio componente ESP-IDF independente**,
direto em `components/` — `hv9910/`, `tps2378/`, `voltage_sense/`,
`eth_init/`, `devid/`, `admin_channel/`, `status_leds/` — cada um com seu
próprio `CMakeLists.txt` e sem nenhum conhecimento em tempo de compilação
de qual placa está rodando: cada `_init()`/`_start()` recebe uma struct
de config (`hv9910_config_t`, `tps2378_config_t`, ...) montada pelo
`app_main()` a partir de `poe_luminaire_main.h` — o mesmo padrão que os
próprios drivers do ESP-IDF usam (`i2c_config_t`, `spi_bus_config_t`,
...). `main/CMakeLists.txt` é o único lugar que declara depender de todos
os sete.

Essa divisão existe pra resolver um problema concreto: um header de
config compartilhado (`poe_luminaire_main.h`) só pode morar em UM lugar,
e `main/` já depende de todo componente pra funcionar — se os
componentes também dependessem de `main/` só pra pegar esse header, o
ESP-IDF rejeitaria o ciclo de dependência no build. Com cada componente
recebendo sua config via struct explícita em vez de incluir o header
compartilhado diretamente, só `main/` conhece `poe_luminaire_main.h`, e a
dependência flui numa única direção (`main` → cada componente).

O ESP-IDF descobre a pasta `components/` na raiz do projeto
automaticamente — não precisa de nenhuma configuração extra
(`EXTRA_COMPONENT_DIRS` etc.) pra isso funcionar, mesmo com vários
componentes lado a lado.

## Verbosidade do log no console

Por padrão, o ESP-IDF imprime bastante ruído de inicialização (banner do
bootloader, dump da tabela de partições, carregamento de segmentos de
imagem, sondagem de heap/flash, etc) antes mesmo do `app_main()` rodar.
Este projeto silencia tudo isso via `sdkconfig.defaults` (nível de log
padrão da aplicação elevado pra `WARN`, nível de log do bootloader
elevado pra `WARN`) e depois reabilita explicitamente o log `INFO` pras
suas próprias sete tags de módulo bem no início do `app_main()` — veja
`quiet_boot_noise()` em
[main/poe_luminaire_main.c](main/poe_luminaire_main.c). Avisos/erros
internos do IDF ainda são impressos; só a conversa rotineira de nível
INFO é suprimida.

## Log de eventos de PoE/AUX/VBUS

Toda vez que o sinal (com debounce) de CDB, T2P, ou "VBUS acima do
limiar" muda de estado, `tps2378` registra isso imediatamente e de
forma independente, não importa se isso vira ou não o veredito geral de
pronto/não-pronto:

```
I (...) TPS2378: EVENT: CDB asserted — real PoE negotiated (inrush done)
W (...) TPS2378: EVENT: T2P dropped — no AUX/Type-2 confirmation anymore
W (...) TPS2378: EVENT: VBUS too low — 18300mV < 40000mV threshold
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

`main/` tem só o entry point + a config da placa; todo o resto do
firmware é um componente independente em `components/` (veja "Estrutura
de componentes" acima):

| Arquivo                                             | Responsabilidade                                                  |
| ---------------------------------------------------- | ---------------------------------------------------------------|
| `main/poe_luminaire_main.c`                          | `app_main()`: monta a config de cada componente e orquestra a sequência de boot — tag de log `MAIN` |
| `main/poe_luminaire_main.h`                          | Mapa de pinos, suposições de hardware, e todos os valores de configuração da aplicação — só incluído por `main/poe_luminaire_main.c` |
| `components/hv9910/hv9910.[ch]`                      | Controle do driver de LED (SHUTDOWN + PWM de dimerização) numa única task/fila de comandos, persistência de brilho/estado em NVS — tag de log `HV9910` |
| `components/tps2378/tps2378.[ch]`                    | Monitoramento de CDB/T2P (PoE/AUX) e VBUS (mediana de 3 amostras + histerese) do TPS2378, watchdog de segurança — tag de log `TPS2378` |
| `components/voltage_sense/voltage_sense.[ch]`        | Leitura de tensão VBUS/VLED (ADC_CH_VLED_P/ADC_CH_VLED_N) — tag de log `VOLT_SENSE` |
| `components/status_leds/status_leds.[ch]`            | Único lugar que controla os LEDs indicadores (azul: energia/tensão, vermelho: estado do driver) — sem dependência de outros componentes, recebe `power_ok_fn`/`driver_on_fn` do `main` por ponteiro de função, task própria |
| `components/eth_init/eth_init.[ch]`                  | Bring-up do PHY IP101G / Ethernet / DHCP, com retry+backoff e sem reinicializações em loop — tag de log `ETH_INIT` |
| `components/eth_init/idf_component.yml`              | Dependência gerenciada do driver de PHY IP101G |
| `components/devid/devid.[ch]`                        | Identidade do dispositivo: serial, chave/época, claim remoto, staging de rotação de chave — tag de log `DEVID` |
| `components/admin_channel/admin_protocol.h`          | Formato de pacote do canal de admin (struct, enums de tipo/status) — sem lógica |
| `components/admin_channel/admin_channel.[ch]`        | Canal UDP de administração autenticado (task própria) — único ponto de controle pela rede (discover, status, on/off/dim, identify, reboot, factory reset, rotação de chave) — tag de log `ADMIN_CH` |
| `tools/lumtool.py`                                   | Ferramenta de host única e autocontida: menu interativo com provisionamento e administração remota |
