# bareiron 3DS

Port do [bareiron](https://github.com/p2r3/bareiron) (servidor minimalista de Minecraft Java 1.21.8) para o Nintendo 3DS.
Os amigos entram pelo Minecraft Java no PC usando o IP do 3DS na mesma rede Wi-Fi.

Como usar: veja o [LEIA-ME.txt](LEIA-ME.txt).

## O que mudou em relação ao bareiron

- `source/platform_3ds.c` (novo): telas, Wi-Fi (`soc:U`), botões, HOME e salvar ao sair.
  O loop do servidor chama `task_yield()` com frequência, e é ali que o 3DS lê os botões e redesenha a tela de cima.
- `include/globals.h`: no 3DS `task_yield()` vira função de verdade.
- `source/main.c`: o `main()` do bareiron vira `bareiron_main()`, e erros aparecem na tela em vez de fechar o app.
- `source/serialize.c`: o mundo fica em `sdmc:/3ds/bareiron3ds/world.bin`.
- `source/tools.c`: o relógio usa o tick do 3DS.
- `source/globals.c`: MOTD próprio.

`source/registries.c` e `include/registries.h` foram gerados com o `extract_registries.sh` do bareiron a partir do `server.jar` 1.21.8.

## Compilar

Com devkitPro (devkitARM + libctru):

```sh
make        # bareiron3ds.3dsx
make cia    # bareiron3ds.cia (precisa de makerom e bannertool no PATH)
```

O GitHub Actions (`.github/workflows/bareiron3ds.yml`) compila os dois e publica o pacote como artefato.

Baseado no bareiron commit `02733be`. Licença GPLv3.
