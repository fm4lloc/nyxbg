# NyxBG

Um renderizador de wallpapers para compositores Wayland que implementam o protocolo `wlr-layer-shell`.

O NyxBG desenha um PNG ou JPEG estático atrás de todas as outras superfícies e então entra em espera. Não é um compositor, componente de desktop, player de slides ou mecanismo de animação. Depois que o wallpaper é enviado ao compositor, o processo permanece bloqueado até que o compositor ou um sinal o acorde.

*Read this in* [English](README.md).

## Design

- ISO C11, POSIX.1-2008. Sem C++, GObject ou framework.
- Quatro componentes obrigatórios: `wayland-client`, `libpng`, `libjpeg` e as descrições do protocolo `wlr-layer-shell`. Sem GTK, Qt, SDL, Cairo, OpenGL ou Vulkan.
- Renderização por software em buffers `wl_shm`. Não há contexto GPU nem render loop.
- Uma responsabilidade por módulo; os módulos se comunicam pelas interfaces em `include/`, nunca por globais.
- Após o commit inicial, o processo bloqueia em um único `poll()` sobre dois descritores: o socket Wayland e um self-pipe de sinais.
- Toda função e todo tipo de dado público possui comentário em estilo kernel-doc.

## Compilação

```sh
make
sudo make install            # instala em /usr/local
```

### Requisitos essenciais

| Requisito | Finalidade |
|---|---|
| GCC 12+ ou Clang 15+ | Compilador C11 |
| `wayland-client` | Biblioteca cliente Wayland |
| `wayland-scanner` | Geração do código de protocolo |
| `libpng` | Decodificação PNG |
| `libjpeg-turbo` | Decodificação JPEG |
| `wlr-protocols` | Descrição do protocolo `wlr-layer-shell` |
| `wayland-protocols` | Descrições auxiliares do protocolo Wayland |

O `wlr-protocols` é preferencialmente obtido do sistema. Quando indisponível, o NyxBG pode utilizar uma cópia vendorizada em `protocol-xml/`.

### Targets e opções

```sh
make                         # build release, warnings são erros
make BUILD=debug             # -Og -g3, depuração em nível de fonte
make BUILD=asan              # AddressSanitizer + UndefinedBehaviorSanitizer
make BUILD=lto               # link time optimization
make WERROR=0                # rebaixa erros para warnings
make analyze                 # analisador estático do GCC
make info                    # informa o que este build realmente usará
make vendor                  # copia os XMLs do sistema para protocol-xml/
make PREFIX=/usr install
make clean
```

### Build rigoroso

O conjunto de warnings não é consultivo. Todo diagnóstico que o compilador pode emitir sobre o código próprio do projeto é habilitado, e `-Werror` permanece ativo nos quatro perfis. Um warning que chega ao build release é um warning que não deve ser ignorado.

Headers de terceiros entram por `-isystem`, e não por `-I`. Portanto, `libpng`, `libjpeg` e `libwayland` não precisam satisfazer as regras de warning do código próprio do projeto.

O código de protocolo gerado pelo `wayland-scanner` é compilado separadamente e não é submetido ao mesmo conjunto de warnings do código escrito manualmente.

Flags dependentes da versão do compilador ou da arquitetura são testadas antes de serem utilizadas. `make info` mostra exatamente quais opções foram aceitas pela toolchain atual.

`WERROR=0` existe como mecanismo de compatibilidade para toolchains que produzam novos diagnósticos. Não é o comportamento padrão.

Hardening permanece ativo em todos os perfis, incluindo `_FORTIFY_SOURCE`, stack protector, stack-clash protection, PIE, full RELRO, stack não executável e controle de fluxo quando disponível na arquitetura alvo.

As opções `-fharden-compares` e `-fharden-conditional-branches` não são utilizadas porque adicionam redundância destinada principalmente a modelos de ameaça envolvendo falhas físicas ou injeção de falhas no processador. `-fstack-protector-all` também não é utilizado porque `-fstack-protector-strong` já cobre os casos relevantes para o NyxBG.

### De onde vem o código de protocolo

`protocol/` contém somente código gerado. É criado por `make`, removido por `make clean` e não é rastreado pelo Git.

O XML usado para geração é procurado em dois lugares, nesta ordem:

1. **Sistema**, via `pkg-config --variable=pkgdatadir` para `wlr-protocols` e `wayland-protocols`. É o comportamento esperado para empacotadores de distribuição e permite que uma atualização das descrições de protocolo chegue ao NyxBG sem arquivos vendorizados.
2. **`protocol-xml/`**, cópias vendorizadas, quando o sistema não possui os arquivos. `wlr-protocols` em particular não é empacotado em todas as distribuições: Arch e Alpine o fornecem; Debian não. `make vendor` popula esse diretório a partir das cópias do sistema.

`make info` mostra qual das duas fontes venceu. Se nenhuma possuir o arquivo, o build para com uma mensagem indicando o pacote necessário.

`xdg-shell.xml` é necessário para exatamente um símbolo: `zwlr_layer_surface_v1` possui uma requisição `get_popup` cujo tipo de argumento é `xdg_popup`, portanto a tabela de interfaces gerada referencia `xdg_popup_interface`, embora o NyxBG nunca crie um popup. Somente seu código privado é gerado; seu header cliente nunca é incluído.

## Uso

```sh
nyxbg wallpaper.png
nyxbg --mode fit --color 101018 photo.jpg
```

| Opção | Significado |
|---|---|
| `-m`, `--mode MODE` | `fill` (padrão), `fit`, `stretch`, `center` |
| `-c`, `--color RRGGBB` | cor desenhada onde a imagem não alcança (padrão `000000`) |
| `-v`, `--verbose` | saída diagnóstica em stderr |
| `-h`, `--help` | uso |
| `-V`, `--version` | versão |

Não há arquivo de configuração nem daemon. O formato da imagem é determinado pelos magic bytes, não pela extensão do arquivo. Consulte `man 1 nyxbg` para a descrição completa.

### Modos de escala

| Modo | Comportamento |
|---|---|
| `fill` | Cobre a saída. O eixo excedente é cortado e centralizado. |
| `fit` | Mostra a imagem inteira. O restante é preenchido com `--color`. |
| `stretch` | Cobre a saída ignorando a proporção da fonte. |
| `center` | Mantém o tamanho original e centraliza a imagem na saída. |

O downscaling usa um filtro triangular separável cujo suporte aumenta com o fator de redução, portanto todo pixel de origem dentro do footprint contribui. Um checkerboard preto/branco de 1 pixel reduzido em 5x converge para um valor uniforme em vez do moiré produzido por bilinear ou nearest. Isso descreve a média dos valores codificados dos samples; o NyxBG não converte para luz linear durante esta etapa de filtragem. A filtragem é feita com alfa pré-multiplicado e o resultado é composto sobre `--color` antes de ser escrito como `XRGB8888` opaco.

As duas passagens nunca constroem um plano intermediário completo. A passagem vertical mantém um anel apenas com as linhas que ainda são necessárias, sobre uma faixa de colunas estreita o suficiente para que o anel permaneça abaixo de um limite fixo de 64 MiB. Assim, o pico de memória é uma constante da fonte, não uma função da altura da imagem ou da largura da saída. Uma fonte 2048x16384 escalada para uma saída de 2560 pixels mantém cerca de 250 KiB de intermediário; o mesmo trabalho consumia 160 MiB antes da existência do anel.

### Sinais

| Sinal | Efeito |
|---|---|
| `SIGHUP` | relê a imagem do disco e redesenha todas as saídas |
| `SIGINT`, `SIGTERM` | libera todos os recursos Wayland e encerra |

Uma recarga que falha não é fatal: o wallpaper que já está na tela permanece.

## Comportamento em runtime

- Cada saída recebe sua própria layer surface na camada background, ancorada nos quatro lados, com zona exclusiva `-1` para que painéis e docks não reduzam sua área.
- A superfície possui região de input vazia, então eventos de ponteiro e toque passam para o que estiver abaixo.
- A região opaca cobre toda a superfície, permitindo que o compositor pule o blending.
- Monitores podem ser conectados e desconectados a qualquer momento. Uma nova saída recebe o wallpaper; uma removida libera sua superfície e seus buffers.
- Uma mudança de resolução chega como configure da layer surface e dispara um redraw. Uma mudança de escala é obtida de `wl_output`.
- Nenhum callback `wl_surface.frame` é solicitado. O NyxBG não anima, portanto não participa do frame clock.

## Modelo de segurança

O NyxBG não executa programas externos, abre sockets de rede, implementa IPC, escreve arquivos de configuração nem modifica a imagem de origem. Ele lê um único arquivo local e conversa com um único socket Unix.

Duas entradas fazem parte do modelo operacional do programa.

**O arquivo de imagem**, porque um wallpaper pode ser baixado ou compartilhado. As dimensões são limitadas (32767 por eixo, 134.217.728 pixels no total), e o limite é aplicado ao tamanho declarado pelo *header*, antes da chamada que poderia alocar a partir dele — `jpeg_start_decompress()` constrói o conjunto completo de coeficientes para uma imagem progressiva, portanto verificar depois seria tarde demais. Chunks auxiliares também são limitados: o padrão do libpng permite que centenas de chunks de texto comprimido em um arquivo pequeno retenham gigabytes, e uma falha de alocação pode ser reportada como warning; por isso o NyxBG limita a 32 chunks de 256 KiB. Metadados textuais incorporados, perfis ICC e EXIF não são interpretados. Isso mantém a decodificação focada nos dados de pixels. Em particular, a orientação EXIF de JPEG não é aplicada; a imagem precisa ser exportada com a orientação desejada já aplicada.

**O compositor** é um processo separado que fornece eventos de protocolo ao cliente. Os tamanhos recebidos por `configure` e o fator recebido de `wl_output.scale` são limitados na entrada, antes de participarem de qualquer aritmética. O tamanho do buffer também é verificado contra `INT32_MAX`, pois o limite por eixo não limita sozinho o produto, e a contagem de pixels é limitada a 2^27 — um buffer de 512 MiB, que ainda deixa espaço para um painel 8K em escala inteira 2. O número de buffers que uma superfície pode manter ao mesmo tempo também é limitado. Valores fora dos limites produzem diagnóstico e o render não prossegue.

## Limitações conhecidas

- **Escala fracionária.** Os buffers são renderizados na escala *inteira* da saída. Sob uma escala fracionária como 1.5, o compositor reamostra nosso buffer em escala 2. Esse fallback permanece fora do caminho nativo de escala fracionária do NyxBG. O suporte coordenado pelo compositor requer `wp_viewporter` e `wp_fractional_scale_v1`, que estão fora do escopo da 1.0.
- **Orientação da imagem.** A orientação EXIF de JPEG não é interpretada. Os pixels são usados conforme armazenados pelo decoder.
- **Gerenciamento de cor.** Perfis ICC incorporados não são interpretados e o NyxBG não realiza gerenciamento de cor específico da saída. O renderer opera sobre a representação RGB fornecida pelo decoder da imagem.
- **Filtragem em luz linear.** O filtro de resampling opera sobre os valores RGB dos samples decodificados em vez de convertê-los primeiro para luz linear. Isso preserva o caminho atual de renderização e é geralmente pouco perceptível em conteúdo fotográfico, embora padrões sintéticos de contraste extremo possam apresentar diferença maior. Um caminho em luz linear não faz parte do default da 1.0.
- **Sem sealing dos buffers.** Os buffers de memória compartilhada não são selados com `F_SEAL_SHRINK`; os compositores devem tratar `SIGBUS` conforme necessário para outros clientes `wl_shm`.
- **Uma imagem para cada saída.** Wallpapers diferentes por saída são responsabilidade de um *wallpaper manager*, que o NyxBG deliberadamente não é.

## Estrutura

```text
nyxbg/
├── include/       um header por módulo
├── src/           uma unidade de tradução por módulo
├── protocol-xml/  descrições de protocolo vendorizadas, usadas quando o
│                  sistema não as fornece
├── protocol/      gerado por make, removido por make clean, não rastreado
├── LICENSE
├── Makefile
├── nyxbg.1
├── README.md
└── README.pt-BR.md
```

| Módulo | Responsabilidade |
|---|---|
| `main.c` | ponto de entrada, CLI, event loop, shutdown |
| `wayland.c` | conexão, registry, globals |
| `layer.c` | criação da layer surface, anchors, configure |
| `output.c` | descoberta de outputs, hotplug, resolução, escala |
| `image.c` | decodificação PNG e JPEG para RGBA |
| `scale.c` | somente geometria; nenhum pixel é tocado aqui |
| `render.c` | alocação de `wl_buffer`, resampling, damage, attach, commit |
| `signal.c` | tratamento de sinais via self-pipe |

`include/signal.h` deliberadamente compartilha nome com o header do sistema. O Makefile adiciona `include/` com `-iquote`, então `"signal.h"` encontra o header do projeto e `<signal.h>` continua encontrando o libc.

## Licença

GNU General Public License, versão 3 ou posterior. Consulte `LICENSE`.

Todo arquivo-fonte possui a tag `SPDX-License-Identifier: GPL-3.0-or-later`, portanto a licença de qualquer arquivo individual é legível por máquina sem precisar analisar o header.

As descrições de protocolo a partir das quais este build gera código possuem suas próprias licenças e avisos de copyright, preservados nos arquivos.

## Autor

Fernando Magalhães — [fm4lloc@gmail.com](mailto:fm4lloc@gmail.com), [nyx-eco@proton.me](mailto:nyx-eco@proton.me)
