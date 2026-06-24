# Proton Drive GUI - Makefile
# Build:     make
# Install:   sudo make install
# Uninstall: sudo make uninstall

APP        := protondrive-gui
HELPER     := pdg-helper
APP_ID     := io.github.jflc.ProtonDriveGUI
PREFIX     ?= /usr/local
BINDIR     := $(PREFIX)/bin
DATADIR    := $(PREFIX)/share
DESKTOPDIR := $(DATADIR)/applications
ICONDIR    := $(DATADIR)/icons/hicolor/scalable/apps

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags gtk4 libcurl libcrypto librnp)
LIBS     := $(shell pkg-config --libs gtk4 libcurl libcrypto librnp) -lcrypt -lpthread

SRC := src/main.cpp src/proton_client.cpp src/proton_crypto.cpp
OBJ := $(SRC:.cpp=.o)

.PHONY: all clean install uninstall helper

all: $(APP) helper

$(APP): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The content-decryption helper is built with Go and uses Proton's GopenPGP,
# which understands Proton Drive's transitional content format that other
# OpenPGP libraries reject.
helper: helper/$(HELPER)

helper/$(HELPER): helper/main.go helper/go.mod
	cd helper && go build -o $(HELPER) .

clean:
	rm -f $(OBJ) $(APP) helper/$(HELPER)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	install -m 755 helper/$(HELPER) $(DESTDIR)$(BINDIR)/$(HELPER)
	install -d $(DESTDIR)$(DESKTOPDIR)
	install -m 644 data/$(APP_ID).desktop $(DESTDIR)$(DESKTOPDIR)/$(APP_ID).desktop
	install -d $(DESTDIR)$(ICONDIR)
	install -m 644 data/$(APP_ID).svg $(DESTDIR)$(ICONDIR)/$(APP_ID).svg
	-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor 2>/dev/null || true
	-update-desktop-database $(DESKTOPDIR) 2>/dev/null || true
	@echo "Installed $(APP). You should now see it in your applications menu."

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)
	rm -f $(DESTDIR)$(BINDIR)/$(HELPER)
	rm -f $(DESTDIR)$(DESKTOPDIR)/$(APP_ID).desktop
	rm -f $(DESTDIR)$(ICONDIR)/$(APP_ID).svg
	-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor 2>/dev/null || true
	-update-desktop-database $(DESKTOPDIR) 2>/dev/null || true
	@echo "Uninstalled $(APP)."
