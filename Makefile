PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR   = $(DESTDIR)$(PREFIX)/bin
APPDIR   = $(DESTDIR)$(PREFIX)/share/applications

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = gtk+-3.0 libxfconf-0

TARGET   = xfce-theme-profiles
SRC      = $(TARGET).c
DESKTOP  = $(TARGET).desktop
VERSION := $(shell sed -n 's/^\#define APP_VERSION *"\(.*\)"/\1/p' $(SRC))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -DAPP_VERSION='"$(VERSION)"' -o $@ $< $$(pkg-config --cflags --libs $(PKGS))

check-deps:
	@pkg-config --exists $(PKGS) || { \
	  echo "Dependências ausentes. No Fedora:"; \
	  echo "  sudo dnf install gcc make gtk3-devel xfconf-devel"; exit 1; }
	@echo "Dependências OK."

install: $(TARGET)
	install -Dm755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(APPDIR)
	sed 's|@BINDIR@|$(PREFIX)/bin|' $(DESKTOP).in > $(APPDIR)/$(DESKTOP)
	chmod 644 $(APPDIR)/$(DESKTOP)
	-update-desktop-database $(APPDIR) 2>/dev/null
	@echo "Instalado: $(TARGET) $(VERSION) em $(PREFIX)"

# Recompila do zero a partir do código-fonte atual e reinstala por cima.
# Perfis e configurações do usuário (~/.config/xfce-theme-profiles) são mantidos.
update: check-deps
	@echo "Versão instalada: $$(test -x $(BINDIR)/$(TARGET) && $(BINDIR)/$(TARGET) --version || echo nenhuma)"
	$(MAKE) clean
	$(MAKE) install
	@echo "Atualizado para $(VERSION). Feche e reabra o app / o Gerenciador de Configurações."

# Remove o programa e o atalho. Mantém os perfis do usuário.
uninstall:
	rm -f $(BINDIR)/$(TARGET) $(APPDIR)/$(DESKTOP)
	-update-desktop-database $(APPDIR) 2>/dev/null
	@echo "Removido. Seus perfis continuam em ~/.config/$(TARGET) (use 'make purge' para apagar)."

# Apaga também os perfis salvos (rode SEM sudo, ou ele usa o home do usuário que chamou o sudo).
purge: uninstall
	@h="$$HOME"; [ -n "$$SUDO_USER" ] && h="$$(getent passwd $$SUDO_USER | cut -d: -f6)"; \
	  rm -rf "$$h/.config/$(TARGET)" && echo "Perfis removidos de $$h/.config/$(TARGET)"

status:
	@echo "Código-fonte : $(VERSION)"
	@echo "Instalado    : $$(test -x $(BINDIR)/$(TARGET) && $(BINDIR)/$(TARGET) --version || echo 'não instalado')"
	@test -f $(APPDIR)/$(DESKTOP) && echo "Atalho       : $(APPDIR)/$(DESKTOP)" || echo "Atalho       : ausente"

clean:
	rm -f $(TARGET)

help:
	@echo "make               compila"
	@echo "make check-deps    verifica dependências"
	@echo "sudo make install  instala (PREFIX=$(PREFIX))"
	@echo "sudo make update   recompila e reinstala por cima (mantém perfis)"
	@echo "sudo make uninstall remove programa e atalho (mantém perfis)"
	@echo "make purge         desinstala e apaga os perfis do usuário"
	@echo "make status        mostra versões instalada e do fonte"

.PHONY: all check-deps install update uninstall purge status clean help
