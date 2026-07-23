m68k_ss = ss.source_set()
m68k_ss.add(when: 'CONFIG_AN5206', if_true: files('an5206.c', 'mcf5206.c'))
m68k_ss.add(when: 'CONFIG_MCF5208', if_true: files('mcf5208.c', 'mcf_intc.c'))
m68k_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-kbd.c', 'next-cube.c'))
m68k_ss.add(when: 'CONFIG_Q800', if_true: files('q800.c', 'q800-glue.c'))
m68k_ss.add(when: 'CONFIG_MACIISI', if_true: files('maciisi.c'))
m68k_ss.add(when: 'CONFIG_M68K_VIRT', if_true: files('virt.c'))
m68k_ss.add(when: 'CONFIG_MC68EZ328', if_true: files('mc68ez328.c'))
m68k_ss.add(when: 'CONFIG_PALM', if_true: files('palm.c'))
m68k_ss.add(when: 'CONFIG_MVME147', if_true: files('mvme147.c'))
m68k_ss.add(when: 'CONFIG_MEGADRIVE', if_true: files('megadrive.c'))
m68k_ss.add(when: 'CONFIG_ELTEC_E17', if_true: files('e17.c'))
m68k_ss.add(when: 'CONFIG_AMIGA', if_true: files('amiga.c', 'a1000.c',
                                                 'a2000.c',
                                                 'a3000.c',
                                                 'a4000.c',
                                                 'a500.c',
                                                 'a500plus.c',
                                                 'a600.c',
                                                 'a3000_sdmac.c',
                                                 'amiga_mobo.c',
                                                 'amiga_custom.c',
                                                 'amiga_a2065.c',
                                                 'amiga_fdc.c', 'amiga_kbd.c',
                                                 'mos8520.c'))

hw_arch += {'m68k': m68k_ss}
