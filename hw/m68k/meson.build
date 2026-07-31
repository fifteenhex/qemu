m68k_ss = ss.source_set()
m68k_ss.add(when: 'CONFIG_AN5206', if_true: files('an5206.c', 'mcf5206.c'))
m68k_ss.add(when: 'CONFIG_MCF5208', if_true: files('mcf5208.c', 'mcf_intc.c'))
m68k_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-kbd.c', 'next-cube.c'))
m68k_ss.add(when: 'CONFIG_Q800', if_true: files('q800.c', 'q800-glue.c'))
m68k_ss.add(when: 'CONFIG_M68K_VIRT', if_true: files('virt.c'))
m68k_ss.add(when: 'CONFIG_MVME147', if_true: files('mvme147.c'))
m68k_ss.add(when: 'CONFIG_AMIGA', if_true: files('amiga.c', 'a1000.c',
                                                 'a1200.c',
                                                 'a2000.c',
                                                 'a3000.c',
                                                 'a4000.c',
                                                 'a500.c',
                                                 'a500plus.c',
                                                 'a600.c',
                                                 'cd32.c',
                                                 'cd32_akiko.c',
                                                 'a3000_sdmac.c',
                                                 'amiga_mobo.c',
                                                 'amiga_custom.c',
                                                 'amiga_a2065.c',
                                                 'amiga_fdc.c', 'amiga_kbd.c',
                                                 'mos8520.c',
                                                 'zorro3.c',
                                                 'z3ram.c',
                                                 'mediator4000.c'))

hw_arch += {'m68k': m68k_ss}
