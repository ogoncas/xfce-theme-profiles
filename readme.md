# Xfce-Theme-Profiles

Gerenciador de perfis de tema leve para o ambiente **XFCE** (escrito em C com GTK3 e `libxfconf`). Permite salvar combinações visuais e alternar entre elas com um clique pela interface gráfica ou via linha de comando.

## O que um perfil gerencia

Cada perfil pode alterar (ou ignorar individualmente) os seguintes componentes:

* **Tema GTK:** `xsettings` (`/Net/ThemeName`)
* **Tema de janelas (xfwm4):** `xfwm4` (`/general/theme`)
* **Tema de ícones:** `xsettings` (`/Net/IconThemeName`)
* **Terminal (`xfce4-terminal`):** Esquema de cores em `~/.config/xfce4/terminal/terminalrc`
* **Editor de texto (`Mousepad`):** Esquema de cores via GSettings (`org.xfce.mousepad.preferences.view`)
* **Lançador (`Rofi`):** Diretiva `@theme` em `~/.config/rofi/config.rasi`
* **Tela de login (`LightDM` - opcional):** Atualiza `theme-name` e `icon-theme-name` em `/etc/lightdm/lightdm-gtk-greeter.conf` via `pkexec` (cria backup automático do original e exige temas instalados no sistema, ex: `/usr/share/themes`).

## Dependências

Para compilar o projeto, você precisará de um compilador C (`gcc`), `make` e dos pacotes de desenvolvimento do **GTK3** e **xfconf**.

* **Fedora:**
  ```
  sudo dnf install gcc make gtk3-devel xfconf-devel
  ```
* **Debian / Ubuntu / Mint:**
  ```
  sudo apt install build-essential libgtk-3-dev libxfconf-0-dev
  ```
* **Arch Linux:**
  ```
  sudo pacman -S base-devel gtk3 xfconf
  ```

*(Nota: O pacote `polkit` é necessário apenas se você utilizar a integração opcional com o LightDM).*

## Compilação e Instalação

Clone o repositório, acesse a pasta e utilize o `Makefile` incluso:

```
# Verifica se as dependências estão instaladas
make check-deps

# Compila o binário
make

# Instala em /usr/local (adiciona entrada no Gerenciador de Configurações do XFCE > Pessoal)
sudo make install
```

### Outros comandos úteis do Makefile

* `sudo make update` — Recompila e reinstala o programa mantendo seus perfis salvos.
* `sudo make uninstall` — Remove o binário e o atalho do menu (mantém seus perfis).
* `make purge` — Desinstala o programa e apaga a pasta de configurações do usuário.
* `make status` — Mostra a versão do código-fonte e do binário instalado.

## Como usar

### Interface Gráfica
Abra **Perfis de Tema** no menu de aplicativos ou dentro do **Gerenciador de Configurações do XFCE** (na categoria *Pessoal*), ou execute `xfce-theme-profiles` sem argumentos.

* **Capturar tema atual:** Preenche o formulário com os temas que você está usando no momento.
* **(não alterar):** Campos deixados nessa opção são ignorados ao aplicar o perfil.
* **Duplo clique** em um perfil na lista aplica-o imediatamente.

### Linha de Comando (CLI)
Ideal para atalhos de teclado ou scripts de automação:

```
# Listar todos os perfis salvos (* indica o ativo)
xfce-theme-profiles --list

# Exibir o nome do perfil ativo no momento
xfce-theme-profiles --active

# Aplicar um perfil específico
xfce-theme-profiles --apply "Nome do Perfil"

# Exibir versão
xfce-theme-profiles --version
```

## Armazenamento

Os perfis criados são salvos em formato INI no diretório padrão de configurações do usuário:
`~/.config/xfce-theme-profiles/profiles.ini`
