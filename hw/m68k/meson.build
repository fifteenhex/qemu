m68k_ss = ss.source_set()
m68k_ss.add(when: 'CONFIG_APOLLO', if_true: files('apollo.c'))
m68k_ss.add(when: 'CONFIG_HP300', if_true: files('hp300.c'))
m68k_ss.add(when: 'CONFIG_Q40', if_true: files('q40.c'))
m68k_ss.add(when: 'CONFIG_AN5206', if_true: files('an5206.c', 'mcf5206.c'))
m68k_ss.add(when: 'CONFIG_MCF5208', if_true: files('mcf5208.c', 'mcf_intc.c'))
m68k_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-kbd.c', 'next-cube.c'))
m68k_ss.add(when: 'CONFIG_Q800', if_true: files('q800.c', 'q800-glue.c'))
m68k_ss.add(when: 'CONFIG_MACIISI', if_true: files('maciisi.c'))
m68k_ss.add(when: 'CONFIG_MACIIFX', if_true: files('maciifx.c'))
m68k_ss.add(when: 'CONFIG_MACIICI', if_true: files('maciici.c'))
m68k_ss.add(when: 'CONFIG_MACIIVX', if_true: files('maciivx.c'))
m68k_ss.add(when: 'CONFIG_MACCLASSICII', if_true: files('macclassicii.c'))
m68k_ss.add(when: 'CONFIG_MACSE30', if_true: files('macse30.c'))
m68k_ss.add(when: 'CONFIG_MACII', if_true: files('macii.c'))
m68k_ss.add(when: 'CONFIG_LC475', if_true: files('lc475.c'))
m68k_ss.add(when: 'CONFIG_MACLC550', if_true: files('maclc550.c'))
m68k_ss.add(when: 'CONFIG_QUADRA630', if_true: files('quadra630.c'))
m68k_ss.add(when: 'CONFIG_QUADRA700', if_true: files('quadra700.c'))
m68k_ss.add(when: 'CONFIG_QUADRA950', if_true: files('quadra950.c'))
m68k_ss.add(when: 'CONFIG_MAC128K', if_true: files('mac128k.c'))
m68k_ss.add(when: 'CONFIG_M68K_VIRT', if_true: files('virt.c'))
m68k_ss.add(when: 'CONFIG_AVME352', if_true: files('avme352.c'))
m68k_ss.add(when: 'CONFIG_MC68EZ328', if_true: files('mc68ez328.c'))
m68k_ss.add(when: 'CONFIG_PALM', if_true: files('palm.c'))
m68k_ss.add(when: 'CONFIG_MVME147', if_true: files('mvme147.c'))
m68k_ss.add(when: 'CONFIG_MVME16X', if_true: files('mvme16x.c'))
m68k_ss.add(when: 'CONFIG_BVME4000', if_true: files('bvme6000.c'))
m68k_ss.add(when: 'CONFIG_SORD_M68MX', if_true: files('sord_m68mx.c'))
m68k_ss.add(when: 'CONFIG_SUN3', if_true: files('sun3.c'))
m68k_ss.add(when: 'CONFIG_ATARIST', if_true: files('atarist.c'))
m68k_ss.add(when: 'CONFIG_MEGADRIVE', if_true: files('megadrive.c'))
m68k_ss.add(when: 'CONFIG_ELTEC_E17', if_true: files('e17.c'))
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
