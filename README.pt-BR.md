# DLLs de interface para o Skimmer: Airspy HF+ e RTL-SDR

*[English](README.md) | Português (Brasil)*

DLLs de interface que permitem usar outros receptores SDR no **CW Skimmer**, no **Skimmer Server** e no **RTTY Skimmer Server** (Afreet Software, VE3NEA):

| DLL | Rádio | Documentação |
|---|---|---|
| `AirspyHfIntf.dll` | Airspy HF+ / HF+ Discovery | Esta página |
| `RtlSdrIntf.dll` | RTL-SDR, incluindo os RTL-SDR Blog V3 e V4 | [README-RTLSDR.pt-BR.md](README-RTLSDR.pt-BR.md) |

**Só quer usar?** Os arquivos compilados estão na pasta [`bin`](bin). Copie a DLL do seu rádio para a pasta do Skimmer; não é preciso instalar mais nada.

O restante desta página trata da DLL do Airspy.

> **Estado:** a `AirspyHfIntf.dll` está funcionando com um Airspy HF+ Discovery real, conforme relato do usuário. Os números de bancada abaixo vêm de um teste com rádio simulado.

## Conteúdo

| Arquivo | Descrição |
|---|---|
| `bin/AirspyHfIntf.dll` | DLL compilada, 32 bits, pronta para usar |
| `bin/AirspyHfIntfConfig.exe` | Opcional: a mesma janela de configuração como programa separado |
| `bin/RtlSdrIntf.dll`, `bin/RtlSdrIntfConfig.exe` | Os mesmos dois arquivos para o RTL-SDR |
| `AirspyHfIntf.c` | Código-fonte da DLL (C, arquivo único) |
| `config/` | Código-fonte da janela de configuração (usado pela DLL e pelo programa separado) |
| `AirspyHfIntf.ini` | Configuração (opcional) |
| `test/mock_airspyhf.c` | `airspyhf.dll` falsa que gera um tom de 10 kHz |
| `test/host.c` | Programa de teste que faz o papel do Skimmer |
| `build.sh` | Script de compilação |
| `third_party/` | libairspyhf e libusb, baixadas pelo `build.sh` (não ficam no repositório) |
| `LEIAME.txt` | Instruções resumidas em texto puro |
| `README.md` | Esta documentação em inglês |
| `RtlSdrIntf.c`, `RtlSdrIntf.ini`, `rtlsdr/`, `test/mock_rtlsdr.c` | A DLL do RTL-SDR, descrita em [README-RTLSDR.pt-BR.md](README-RTLSDR.pt-BR.md) |

## Instalação

### Dependências

Nenhuma. A biblioteca do rádio (libairspyhf) e a libusb estão embutidas na DLL, que continua sendo de 32 bits porque o Skimmer é um programa de 32 bits. Não é preciso copiar `airspyhf.dll` nem `libusb-1.0.dll`.

O rádio precisa estar com o driver WinUSB, que o Windows 10/11 instala sozinho para o HF+ Discovery.

### Skimmer Server e RTTY Skimmer Server

1. Copie `AirspyHfIntf.dll` (e opcionalmente `AirspyHfIntf.ini`) para a pasta do programa (`SkimSrv` ou `RttySkimServ`). Não precisa renomear: o programa carrega as DLLs que encontra ali. O RTTY Skimmer Server usa a mesma interface de DLL, mas não foi testado com esta DLL.
2. O rádio aparece na lista como **Airspy HF+**.
3. Apenas 1 receptor, ou seja, uma banda por rádio.

### CW Skimmer

1. Copie a mesma DLL para a pasta do CW Skimmer com o nome **`Qs1rIntf.dll`** (guarde a original, se existir).
2. Em *Settings > Radio*, escolha **QS1R**.
3. O `.ini` continua se chamando `AirspyHfIntf.ini`.

Esse procedimento não foi confirmado: há indícios de que o CW Skimmer 2.1 acessa o QS1R direto por USB (erro "Unable to load libusb0.dll") e não carrega esta DLL. No Skimmer Server a DLL é carregada normalmente.

O rádio só pode ser aberto por um programa de cada vez (feche o SDR# antes).

## Configuração

Os ajustes ficam no `AirspyHfIntf.ini`, ao lado da DLL. A janela de configuração grava nesse arquivo, e você também pode editá-lo à mão. Nos dois casos a DLL percebe a mudança e aplica com o Skimmer recebendo; só um novo número de série espera a próxima vez que o rádio for iniciado.

### Janela de configuração

![Janela de configuração do Airspy HF+](docs/airSpy_window.jpg)

A janela faz parte da DLL e abre sozinha quando o Skimmer inicia o rádio. Ela roda em thread própria, então o Skimmer continua funcionando com ela aberta. Desmarque **Mostrar esta janela quando o rádio iniciar** (ou use `ShowWindow=0`) se não quiser que ela apareça.

- **Número de série:** lista os rádios encontrados, para escolher um quando houver mais de um. Também dá para digitar o número de série.
- **Entrada de RF:** AGC de HF, limiar do AGC, atenuador (com o AGC desligado) e pré-amplificador.
- **Saída para o Skimmer:** ganho digital, deslocamento de frequência, inversão de Q e arquivo de log.
- **Aplicar** grava sem fechar, para você acompanhar o efeito no waterfall.

A janela aparece em português quando o Windows está em português e em inglês nos demais casos.

O `AirspyHfIntfConfig.exe` abre a mesma janela sem o Skimmer, o que ajuda quando a janela foi desativada ou o rádio configurado não abre. Copie-o para a pasta da DLL. Para editar outro arquivo, passe o caminho na linha de comando: `AirspyHfIntfConfig.exe AirspyHfIntf_2.ini`. Se a pasta ficar em `Program Files`, ele oferece reiniciar como administrador, que essa pasta exige para gravar.

### Mais de um rádio

Faça uma cópia da DLL com outro nome terminado em `Intf.dll`, por exemplo `AirspyHfIntf_2.dll`, e crie um `.ini` com o mesmo nome (`AirspyHfIntf_2.ini`) contendo o número de série do outro rádio. A DLL usa o `.ini` que tem o nome dela e recorre ao `AirspyHfIntf.ini` quando esse arquivo não existe. Com número de série definido, o nome do rádio mostrado pelo Skimmer termina com os 8 últimos dígitos.

### Chaves do `.ini`

| Chave | Padrão | Função |
|---|---|---|
| `HfAgc` | 1 | AGC de HF do rádio. Com AGC ligado, `HfAtt` é ignorado |
| `HfAgcThreshold` | 0 | Limiar do AGC: 0 = baixo, 1 = alto |
| `HfAtt` | 0 | Atenuador manual, 0 a 8 (passos de 6 dB) |
| `HfLna` | 0 | Pré-amplificador |
| `GainDb` | 0 | Ganho digital em dB nas amostras entregues ao Skimmer |
| `InvertQ` | 0 | Ponha 1 se o espectro aparecer espelhado |
| `FreqOffsetHz` | 0 | Somado à frequência pedida (transverter/upconverter) |
| `Serial` | vazio | Número de série em hexadecimal, para escolher um rádio específico |
| `ShowWindow` | 1 | 1 abre a janela de configuração quando o Skimmer inicia o rádio |
| `Log` | 0 | 1 grava `AirspyHfIntf.log` na pasta da DLL |

## Como funciona

### A interface do Skimmer

O Skimmer carrega a DLL e usa seis funções exportadas (`stdcall`, nomes sem decoração), definidas pelo VE3NEA na unit `SdrTypes`:

| Função | O que esta DLL faz |
|---|---|
| `GetSdrInfo` | Devolve o nome "Airspy HF+", 1 receptor e as taxas exatas 48000/96000/192000 Hz |
| `StartRx` | Abre o rádio, configura e começa a entregar I/Q |
| `StopRx` | Para a recepção e fecha o rádio |
| `SetRxFrequency` | Sintoniza a frequência central (só o receptor 0) |
| `SetCtrlBits` | Nada (exigida pela interface) |
| `ReadPort` | Devolve 0 (exigida pela interface) |

Em `StartRx` o Skimmer passa uma estrutura com a taxa desejada (`RateID`: 0 = 48 kHz, 1 = 96 kHz, 2 = 192 kHz) e os callbacks. A DLL usa dois:

- **`IqProc`**: chamado 93,75 vezes por segundo com um vetor de 8 ponteiros para blocos de amostras complexas `float` alinhados em 16 bytes. O bloco tem taxa / 93,75 amostras: 512, 1024 ou 2048.
- **`ErrorProc`**: chamado com uma mensagem quando algo falha.

### O caminho das amostras

1. A libairspyhf (ligada estaticamente, junto com a libusb) abre o rádio por USB.
2. A DLL escolhe a menor taxa do rádio que seja a taxa pedida vezes 2ⁿ. No HF+ Discovery isso dá 192 kHz nativo.
3. Cada divisão por 2 passa por um filtro FIR meia-banda de 63 taps (janela de Kaiser, β = 8).
4. As amostras do rádio vêm em `float` ±1,0 e são multiplicadas por 2³¹, a mesma escala de inteiro de 32 bits que o HermesIntf entrega ao Skimmer.
5. Os 7 ponteiros de receptores não usados apontam para um bloco de zeros.

### Detalhes de robustez

- `RateID` é mascarado com `0xFF`, porque o Skimmer Server 1.1+ envia lixo nos bytes altos.
- `StopRx` fecha o rádio em uma thread separada, para não travar o Skimmer caso a thread de amostras esteja dentro de `IqProc`.
- `SetRxFrequency` pode chegar antes ou depois de `StartRx`; a frequência fica guardada e é aplicada na abertura.
- Uma thread pequena observa a data de modificação do `.ini` e reaplica os ajustes quando ela muda.

## Resultado do teste simulado

| Taxa | Blocos/s (esperado 93,75) | Tom medido | Amplitude |
|---|---|---|---|
| 48 kHz | 93,98 | 10 000,0 Hz | igual à esperada |
| 96 kHz | 93,89 | 10 000,0 Hz | igual à esperada |
| 192 kHz | 93,70 | 10 000,0 Hz | igual à esperada |

## Ajustes com o rádio real

- **Espectro espelhado:** se os sinais aparecerem do lado errado da frequência central, use `InvertQ=1`.
- **Nível:** se o waterfall ficar fraco ou saturado, ajuste `GainDb`.
- **Calibração:** as taxas exatas informadas são as nominais; o ajuste fino fica na calibração de frequência do próprio Skimmer.
- **Em caso de falha:** ligue `Log=1` e consulte `AirspyHfIntf.log`.

## Compilar

Com MinGW de 32 bits, rode `sh build.sh`. Ele baixa a libairspyhf e a libusb estática para `third_party/` e gera `AirspyHfIntf.dll` e `AirspyHfIntfConfig.exe` em `bin/`, cada um em arquivo único e sem dependências. Faz o mesmo para `RtlSdrIntf.dll` e `RtlSdrIntfConfig.exe`, usando o driver da RTL-SDR Blog. Precisa de `git`, `curl` e `7z`.

Sem `-DSTATIC_AIRSPYHF`, a DLL é gerada na variante que carrega uma `airspyhf.dll` externa de 32 bits; é essa variante que o teste simulado usa.

Para refazer o teste simulado:

```
i686-w64-mingw32-windres -I config config/settings.rc -O coff -o res.o
i686-w64-mingw32-gcc -O2 -msse2 -shared -static-libgcc -o Qs1rIntf.dll AirspyHfIntf.c config/settings_dialog.c res.o -lcomctl32 -Wl,--kill-at
i686-w64-mingw32-gcc -O2 -shared -o airspyhf.dll test/mock_airspyhf.c
i686-w64-mingw32-gcc -O2 -o host.exe test/host.c
host.exe
```

## Código de terceiros

Os arquivos compilados contêm a libusb, a libairspyhf e o driver da RTL-SDR Blog. As licenças estão listadas em [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Créditos

Plugin por PY3OW e PY2PLL+PY3CRX.

## Referências

- Interface original: unit `SdrTypes` e mensagens do VE3NEA (2009-2010)
- [k3it/HermesIntf](https://github.com/k3it/HermesIntf): DLL aberta equivalente para rádios HPSDR
- [airspy/airspyhf](https://github.com/airspy/airspyhf): biblioteca do Airspy HF+
- [rtlsdrblog/rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog): driver do RTL-SDR com suporte ao V3 e ao V4
