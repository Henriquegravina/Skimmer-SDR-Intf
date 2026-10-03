# RtlSdrIntf (RTL-SDR)

*[English](README-RTLSDR.md) | Português (Brasil)* · [Voltar ao README principal](README.pt-BR.md)

DLL de interface que permite usar um dongle **RTL-SDR**, incluindo os **RTL-SDR Blog V3 e V4**, no **Skimmer Server** e no **RTTY Skimmer Server**. O CW Skimmer não é suportado.

> **Estado:** testada apenas com um driver simulado. Ainda não foi experimentada com um dongle real.

## O que é preciso

- `bin/RtlSdrIntf.dll`. O driver (o fork da RTL-SDR Blog da librtlsdr) e a libusb estão embutidos, então nenhuma outra DLL é necessária.
- O **driver WinUSB** instalado para o dongle com o [Zadig](https://zadig.akeo.ie/), a mesma preparação que o SDR# usa: escolha "Bulk-In, Interface (Interface 0)" e instale o WinUSB.
- Nenhum outro programa usando o dongle ao mesmo tempo.

## Instalação

- **Skimmer Server / RTTY Skimmer Server:** copie `RtlSdrIntf.dll` (e opcionalmente `RtlSdrIntf.ini`) para a pasta do programa. O rádio aparece na lista como **RTL-SDR**. Apenas 1 receptor, ou seja, uma banda.
- **Junto com a DLL do Airspy:** os programas carregam todas as DLLs da pasta ao mesmo tempo, então com a `AirspyHfIntf.dll` lá também os dois rádios aparecem na lista e você escolhe o que estiver com o hardware conectado. Não precisa renomear nada. Veja [As duas DLLs na mesma pasta](README.pt-BR.md#as-duas-dlls-na-mesma-pasta).
- **CW Skimmer:** não é suportado. Ele não consegue carregar a DLL, e renomear não resolve, como explicado no [README principal](README.pt-BR.md#cw-skimmer).

## Recepção de HF

| Dongle | O que "Automática" faz | Observações |
|---|---|---|
| RTL-SDR Blog **V3** | Amostragem direta no ramo Q abaixo de 24 MHz, sintonizador acima | Não precisa ajustar nada |
| RTL-SDR Blog **V4** | Upconverter interno abaixo de 28,8 MHz, sintonizador acima | Não precisa ajustar nada |
| Dongle genérico com upconverter externo | Sintonizador | Ponha em `FreqOffsetHz` o oscilador do upconverter, por exemplo 125000000 |
| Dongle genérico modificado para amostragem direta | — | Escolha "Amostragem direta, ramo I" ou "ramo Q", conforme a ligação feita |

Na amostragem direta o sintonizador fica fora do caminho, então o ganho do sintonizador não tem efeito e o **AGC digital do RTL2832** é o único controle de ganho. Nesse modo também não há filtragem antes do conversor: emissoras fortes de radiodifusão podem sobrecarregá-lo, e um filtro passa-faixa para a banda em uso ajuda muito.

## Largura de banda

A largura de espectro que o Skimmer enxerga é a taxa de amostragem escolhida no próprio Skimmer: 48, 96 ou 192 kHz. A DLL já limita o sinal a essa largura; os filtros digitais dela removem tudo o que está fora antes de entregar as amostras.

O que dá para ajustar é a **largura do filtro do sintonizador**, o filtro analógico que fica antes do conversor de 8 bits. Por padrão o driver usa a largura da taxa de amostragem, ou seja, 1 MHz ou mais. O Skimmer precisa de no máximo 192 kHz, então a opção mais estreita (350 kHz) mantém sinais fortes de fora da banda do Skimmer longe do conversor, que é onde um RTL-SDR sobrecarrega. Vale sempre que o sintonizador está em uso: sempre no V4 e acima de 24 MHz no V3. Na amostragem direta o sintonizador fica fora do caminho e o ajuste não tem efeito.

## Taxa de amostragem e decimação

O conversor do dongle tem só 8 bits, mas amostra muito mais rápido do que o Skimmer precisa. A DLL filtra e decima esse fluxo até a taxa do Skimmer, e fazer a média de D amostras em uma vale cerca de log2(D)/2 bits a mais. **Taxa de amostragem**, na janela, escolhe a velocidade em que o dongle amostra, e a linha abaixo dela mostra a decimação e a resolução resultante em cada taxa do Skimmer:

| Taxa do dongle | Para o Skimmer em 192 kHz | em 96 kHz | em 48 kHz |
|---|---|---|---|
| 1,152 MS/s | ÷6, cerca de 9,3 bits | ÷12, 9,8 bits | ÷24, 10,3 bits |
| 1,536 MS/s (padrão) | ÷8, 9,5 bits | ÷16, 10,0 bits | ÷32, 10,5 bits |
| 1,920 MS/s | ÷10, 9,7 bits | ÷20, 10,2 bits | ÷40, 10,7 bits |
| 2,304 MS/s | ÷12, 9,8 bits | ÷24, 10,3 bits | ÷48, 10,8 bits |
| 3,072 MS/s | ÷16, 10,0 bits | ÷32, 10,5 bits | ÷64, 11,0 bits |

Dois pontos a ter em mente. Acima de cerca de 2,4 MS/s um RTL-SDR pode perder amostras na USB, então 3,072 MS/s vale o teste, mas não é garantido. E o ganho é em resolução para sinais fracos, não em sobrecarga: um sinal forte o bastante para saturar o conversor de 8 bits continua saturando, e para isso servem o ganho e a largura do filtro do sintonizador.

Uma nova taxa de amostragem vale na próxima vez que o Skimmer iniciar o rádio.

## Bias-T

Marcar **Bias-T** coloca alimentação DC (cerca de 4,5 V no V3 e no V4) no conector de antena, para alimentar uma antena ativa ou um pré-amplificador. Vem desligado. Não ligue com uma antena que seja curto-circuito em DC. A DLL desliga o Bias-T de novo quando o Skimmer para o rádio.

## Medidores de nível

Com o Skimmer recebendo, duas barras na janela de configuração mostram o nível de pico das amostras **I** e **Q** direto da saída do conversor de 8 bits do dongle, em dBFS. 0 dBFS indica que o conversor chegou ao código 0 ou 255, ou seja, o sinal está ceifando, e o **CLIP** acende em vermelho por 3 segundos. A barra fica verde até -12 dBFS, amarela até -3 dBFS e vermelha acima disso; a marca branca e o número à direita são o maior pico dos últimos 2 segundos.

A medida é feita antes de qualquer filtragem e antes do `GainDb`, então mostra o que o conversor enxerga, incluindo sinais fortes fora da banda do Skimmer. Mantenha os picos abaixo de cerca de -3 dBFS. Se acender CLIP:

- com o sintonizador em uso, reduza o **ganho do sintonizador** (ou desligue o AGC do sintonizador e escolha um valor menor) e experimente o filtro do sintonizador mais estreito;
- na amostragem direta, desligue o **AGC digital do RTL2832** e, se ainda ceifar, coloque um atenuador ou um filtro passa-faixa antes do dongle.

Na amostragem direta só um ramo leva o sinal, então uma das barras fica perto do mínimo. Isso é esperado.

Com o rádio parado, e no `RtlSdrIntfConfig.exe`, as barras ficam vazias.

## Configuração

Os ajustes ficam no `RtlSdrIntf.ini`, ao lado da DLL. A janela de configuração faz parte da DLL e abre sozinha quando o Skimmer inicia o rádio; ela grava nesse arquivo, e você também pode editá-lo à mão. As mudanças valem com o Skimmer recebendo. Só a escolha do dongle espera a próxima vez que o rádio for iniciado.

O `RtlSdrIntfConfig.exe` abre a mesma janela sem o Skimmer.

![Janela de configuração do RTL-SDR](docs/rtl_sdr_window.jpg)

| Chave | Padrão | Função |
|---|---|---|
| `DeviceIndex` | -1 | Qual dongle abrir; -1 = o primeiro encontrado |
| `Serial` | vazio | Número de série do dongle escolhido |
| `HfMode` | 0 | 0 = automática, 1 = amostragem direta no ramo I, 2 = amostragem direta no ramo Q |
| `SampleRate` | 1536000 | Taxa de amostragem do dongle: 1152000, 1536000, 1920000, 2304000 ou 3072000 |
| `TunerAgc` | 1 | AGC do sintonizador. Desligado, vale o `TunerGain` |
| `TunerGain` | 297 | Ganho do sintonizador em décimos de dB (0 a 496); é usado o valor suportado mais próximo |
| `TunerBandwidthHz` | 0 | Largura do filtro analógico do sintonizador: 0 = automática (da largura da taxa de amostragem), ou 350000 a 1550000 |
| `RtlAgc` | 0 | AGC digital do RTL2832 |
| `BiasTee` | 0 | 1 liga o Bias-T |
| `Ppm` | 0 | Correção de frequência em ppm |
| `GainDb` | 0 | Ganho digital em dB nas amostras entregues ao Skimmer |
| `InvertQ` | 0 | Ponha 1 se o espectro aparecer espelhado |
| `FreqOffsetHz` | 0 | Somado à frequência pedida (upconverter externo ou transverter) |
| `ShowWindow` | 1 | 1 abre a janela de configuração quando o Skimmer inicia o rádio |
| `Log` | 0 | 1 grava `RtlSdrIntf.log` na pasta da DLL |

Muitos dongles saem de fábrica com o mesmo número de série (`00000001`). Por isso a janela grava a posição e o número de série: a DLL abre o dongle naquela posição se o número de série conferir e, caso contrário, procura pelo número de série.

Para usar mais de um dongle, faça uma cópia da DLL com outro nome, por exemplo `RtlSdrIntf_2.dll`, com o seu próprio `RtlSdrIntf_2.ini`.

## Como funciona

1. O dongle roda na taxa de amostragem escolhida (1,536 MS/s por padrão). Todas as taxas oferecidas são exatas para o cristal de 28,8 MHz e múltiplas de 192 kHz.
2. As amostras de 8 bits são convertidas para ponto flutuante e o offset DC do conversor é removido, para não aparecer como uma portadora no meio do waterfall.
3. Uma cadeia de estágios baixa a taxa para 192, 96 ou 48 kHz: primeiro um estágio de divisão por 3 ou por 5, quando o fator pede, e depois estágios de divisão por 2. Os primeiros usam filtros FIR curtos e o último um filtro meia-banda de 127 taps. A rejeição de alias calculada é de pelo menos 76 dB nos 92% centrais da banda, em todas as combinações de taxas.
4. As amostras são entregues em blocos de 2048, 1024 ou 512, 93,75 vezes por segundo, na mesma escala de inteiro de 32 bits das outras DLLs de interface.

O restante (a interface do Skimmer, o fechamento do rádio em thread separada, a releitura do `.ini`) é igual ao da DLL do Airspy, descrito no README principal.

## Resultado do teste simulado

Com um driver falso enviando um tom de 10 kHz a metade do fundo de escala e um programa de teste fazendo o papel do Skimmer:

| Taxa | Blocos/s (esperado 93,75) | Tom medido | Amplitude |
|---|---|---|---|
| 48 kHz | 93,66 | 10 000,0 Hz | igual à esperada |
| 96 kHz | 94,03 | 10 000,0 Hz | igual à esperada |
| 192 kHz | 93,91 | 10 000,0 Hz | igual à esperada |

Mudar o modo de HF, o ganho do sintonizador, o Bias-T e a correção de frequência pela janela chegou ao driver durante a recepção, e o Bias-T foi desligado quando o rádio parou.

## O que conferir com um dongle real

- **Espectro espelhado:** se os sinais aparecerem do lado errado da frequência central, marque **Inverter Q**. Isso pode ser diferente entre o sintonizador e a amostragem direta.
- **Nível:** se o waterfall ficar fraco ou saturado, ajuste primeiro o ganho do sintonizador e depois o `GainDb`.
- **Frequência:** use `Ppm` em dongles sem TCXO; o V3 e o V4 normalmente não precisam.
- **Em caso de falha:** ligue `Log=1` e consulte `RtlSdrIntf.log`.

Para refazer o teste simulado, compile a DLL com `test/mock_rtlsdr.c` no lugar dos fontes do driver real.
