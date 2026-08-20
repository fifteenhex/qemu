# Default configuration for m68k-softmmu

# Boards are selected by default, uncomment to keep out of the build.
# Cut down to just the AVME-352 serial card: it needs no graphics, no
# networking and no block devices.
CONFIG_AN5206=n
CONFIG_MCF5208=n
CONFIG_NEXTCUBE=n
CONFIG_Q800=n
CONFIG_M68K_VIRT=n
CONFIG_AVME352=y
