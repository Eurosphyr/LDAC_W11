# ldac-win — áudio LDAC do Windows para o fone Bluetooth

Programa de linha de comando (não é um driver) que captura o que o Windows está tocando
(WASAPI loopback), codifica em **LDAC** (libldac da Sony) e envia ao fone via **A2DP**
(BTstack), usando o seu dongle USB Bluetooth diretamente.

```
Windows (apps) → saída de áudio escolhida → WASAPI loopback → LDAC → A2DP/BTstack → dongle (WinUSB) → fone
```

## Status honesto
- Compila e gera o `.exe` (PE x64, estático, só DLLs do próprio Windows).
- O encoder LDAC foi testado isoladamente: 990/660/330 kbps a 48 kHz e 909/606/303 kbps a 44,1 kHz, pacotes de 660 bytes.
- **Não foi testado com o seu fone e o seu dongle** (não tenho acesso ao hardware). A parte Bluetooth
  (pareamento, negociação do codec, estabilidade do link) é a que pode precisar de ajuste.
  Se falhar, o arquivo `ldac-win_hci.pklg` (abre no Wireshark) e a saída do console mostram onde parou.

## 1. Preparar o dongle (uma vez)
1. Baixe o **Zadig** (https://zadig.akeo.ie) e abra.
2. *Options → List All Devices*.
3. Selecione o dongle (aparece como "CSR8510 A10", "Bluetooth Radio" ou similar, USB ID 0A12:0001).
4. Escolha o driver **WinUSB** e clique em *Replace Driver*.

Efeito: esse dongle deixa de funcionar como Bluetooth normal do Windows (mouse/teclado Bluetooth nele param).
Para desfazer: Gerenciador de Dispositivos → o dongle → Desinstalar dispositivo (marcando "excluir o driver")
→ desconectar e reconectar.

## 2. Usar
Abra o Prompt de Comando na pasta do programa:

```
ldac-win.exe --scan
```
Coloque o fone em **modo de pareamento**. Anote o endereço (ex.: `AA:BB:CC:DD:EE:FF`) e feche com Ctrl+C.

```
ldac-win.exe --list-audio
```
Mostra as saídas de áudio do Windows. O programa captura o som de **uma** delas.

```
ldac-win.exe --connect AA:BB:CC:DD:EE:FF --device 0 --quality sq
```
Depois toque algo no Windows nessa saída. A cada 5 s aparece uma linha `[stream] ...` com o bitrate.

Opções: `--quality hq|sq|mq` (990 / 660 / 330 kbps; padrão `sq`, mais estável), `--no-retry`.
Nas próximas vezes o fone já está pareado: ligue-o e rode só o `--connect`.

## Requisitos e cuidados
- A saída de áudio capturada precisa estar em **44100, 48000, 88200 ou 96000 Hz**
  (Configurações → Sistema → Som → dispositivo → Propriedades → Avançado). 192 kHz não funciona.
- O som continua saindo também pela saída capturada. Para não ouvir em dobro, use uma saída sem caixas
  (ex.: HDMI do monitor sem som) ou um cabo virtual (VB-Cable) como saída do Windows.
- O volume é o do próprio fone. O volume do Windows pode não afetar o que é capturado.
- A latência é maior que a de um driver nativo (captura + codec + buffer): ruim para jogos/vídeo sem compensação.
- Dongles "5.3" com ID CSR costumam ser clones; se o link ficar instável, tente `--quality mq`.
- O fone precisa anunciar LDAC. O programa lista os codecs que ele oferece ao conectar.
- O `.exe` não é assinado digitalmente: o Windows SmartScreen/antivírus pode avisar.

## Compilar
- Windows: MSYS2 MINGW64 → `./build_msys2.sh`
- Linux (cross): `./build_linux_cross.sh`

## Licenças
BTstack (BlueKitchen, licença para uso não comercial), libldac (Apache-2.0, Sony/AOSP). Veja `third_party/`.
