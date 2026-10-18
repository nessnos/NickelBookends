include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickelbookends.so
override SOURCES  += src/nickelbookends.cc src/nb_calc.cc
override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden

override PKGCONF  += Qt5Widgets Qt5Sql

ifeq ($(NB_CONFIG_DIR),)
override NB_CONFIG_DIR := /mnt/onboard/.adds/nickelbookends
endif
override CPPFLAGS += -DNB_CONFIG_DIR='"$(NB_CONFIG_DIR)"'

override KOBOROOT += res/settings.ini:$(NB_CONFIG_DIR)/settings.ini res/uninstall:$(NB_CONFIG_DIR)/uninstall res/nm-nickelbookends:/mnt/onboard/.adds/nm/nickelbookends

include NickelHook/NickelHook.mk
