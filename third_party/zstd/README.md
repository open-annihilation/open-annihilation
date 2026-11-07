# zstd

Zstandard compression, which a replay uses to store its recording.

- Source: <https://github.com/facebook/zstd/releases/tag/v1.5.7>,
  `zstd-1.5.7.tar.gz`, version 1.5.7, SHA-256
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`.
  The files copied from that release are `lib/zstd.h`,
  `lib/zstd_errors.h`, `lib/common/`, `lib/compress/`,
  `lib/decompress/` and `LICENSE`. Each copied file's SHA-256:

- `LICENSE` `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8`
- `common/allocations.h` `6a718e8edaca112abdc0bdfa7de67edefaa44c8d9e514b79672ab58ab3a9118a`
- `common/bits.h` `5bb7693a35d7ce03f8715c0b1e237062285e052250fdd34f72f38e7e92365090`
- `common/bitstream.h` `a9461685c9add6f609078acc950a08db4c02e6e6a849d550f45182ba2ae38556`
- `common/compiler.h` `d19829705d8039437ffba873df2b58d20d7b876d58da479121d2e515df55cbf7`
- `common/cpu.h` `23d520b536a6f23114e66549d03a967b1f24b15560c5b1ebd47ee392ccee4a1c`
- `common/debug.c` `7ab1ea104acf3243fc1cd4e6d2514046f1dfc1101c7c676145c8da080e8c24ca`
- `common/debug.h` `8260cd5087b8309679c61b772b535688993c61a675a30481b8ae55b45740ba15`
- `common/entropy_common.c` `7dfce29c6bc807645b1f0c373dad94bbb49603b175a84e72d1739bf9df65feb8`
- `common/error_private.c` `3f5873b2626ca1cdf554b7ec7b04ea0ef296fd0f6882d7fad42448b7e2dbb41c`
- `common/error_private.h` `ae077bb433eb150ee247410a0357f910b9c07be839c6a9abe0b2e6b18ceb716d`
- `common/fse.h` `5235ce1e512bf80204d013b1f4cecd776fdaa9effddb48308bce08f6d0b84499`
- `common/fse_decompress.c` `84eba7030a036b36363a5108e760428ce549f1a833bec6464dc755f6ed51a6fb`
- `common/huf.h` `9a83d899c8c9bf03389d482562090ec2443bc0f87f27864a2b891b180333f2d4`
- `common/mem.h` `4dd5fe76fa30a020cd4b3af3134ae143920300a1fe54baac1a5da73297e66783`
- `common/pool.c` `9431e26cf7ca46ffb878f17df31198046664d592ae208dfbe3715e82b87af79d`
- `common/pool.h` `bca19a2408e85f31bad3a13f205de92ac26ecda0c5af96fdd7f213ce03d9fc78`
- `common/portability_macros.h` `75e2b43968b70decc6bd17876833be04894035594ddc1e745cd6b70be8265637`
- `common/threading.c` `50448323b46a8e1bde042ee88ec44fcfc646f3d4aeba8a06be54d26a710ed78d`
- `common/threading.h` `9ca63027ea64046acacccbe056a414d37ea7ea428ce299fe08abe53c204c5c65`
- `common/xxhash.c` `8444d064922f434b67b708a987981aa21dfeece735b48c82d31f7027d0ccef03`
- `common/xxhash.h` `8cb837b21a8fe9a6b9dbcd0961ab16e733bfcbfa9e003f3a496ce07ae80aa8ee`
- `common/zstd_common.c` `f49eb8023a90d3de925373bd9ee0d36ed46b26c529840b64220397446cd73795`
- `common/zstd_deps.h` `9c77ea7d0afa8d5c868a7839e178a446a82d88eb75fd56f2fd7d9d96c5ab7c11`
- `common/zstd_internal.h` `ad3d95ce2b81a8c5c6b00cfe6323d8e80cacb690d3b5883725cd0d939f4f576f`
- `common/zstd_trace.h` `17f6daa4c7e97055cd1ed7bcf4d9241a4e79b465f083c5d732fda1df2f0be11e`
- `compress/clevels.h` `d764daa89b7a636d26fe96ff3536c5075e9867fb5dddbf8c9d57023a8ff4a155`
- `compress/fse_compress.c` `5070807a489757b87c1e1f50332b099e8ffe22971228218111eef64d3321ae82`
- `compress/hist.c` `1a613082642b9cabdeecd3b5929f7230cbb2f8344a28f5010f3aec12cb647ab8`
- `compress/hist.h` `9e3363e69d5fa35c1f6e8d2970c6c2453c06fd8a8ab3cc516a14c77d07cdea35`
- `compress/huf_compress.c` `0be78471f5175e49bbacb898d8b107bb8f7e13adc84780e0625be5861f016086`
- `compress/zstd_compress.c` `10c315ae609d49b2fc431521aa95908086e69fd917991c5ed9a875af3d1208e8`
- `compress/zstd_compress_internal.h` `ab7754413cef565da559bda7eb738bc2b3708c48877cb9ec37aff767d32c57d1`
- `compress/zstd_compress_literals.c` `a8f25f1bd4c0d2d9dda3355d2103c9dfbd0b7056bfd373e29e923662bfea5596`
- `compress/zstd_compress_literals.h` `02270cd6bc060279217861776a53adb67f8d7e54f47f59d2a4ddcef2e705fe2d`
- `compress/zstd_compress_sequences.c` `3f4334c19ed770c4007bc12df4098026d4eb1a6fbebfbf36df58054e7babc3e9`
- `compress/zstd_compress_sequences.h` `361d108d7d452ed26f0ae4b39964f8e62166ed91d03347a5229e5d228734efc8`
- `compress/zstd_compress_superblock.c` `dd62ed79ee1feec78b2e5bfc63417ccbdcda426f4220d112b0fcd8a9d0c7e667`
- `compress/zstd_compress_superblock.h` `6b097999ea1d91776ca5a12e5ee02c1ed15edfcd31ba07b9ada94b0955195677`
- `compress/zstd_cwksp.h` `ed77c2739b18b9800b97785262526a1dd4f385311ca5c2caa20b6f2a35f3735d`
- `compress/zstd_double_fast.c` `9f6d003c2fe1208737bec685910324220424ba47d0323fa30d9d5f53c50e91da`
- `compress/zstd_double_fast.h` `d9c29c9c004f572532265e12af78d5949c0908f4ef0bb0158977bcf80e1cf92b`
- `compress/zstd_fast.c` `8037320f321836652e73ab6cbf7b8f1f4c97cef38835ac6b4aaf0af716aa398d`
- `compress/zstd_fast.h` `dffdf43fae7ee10a2acc04e6a5eaaf03f7d3572898117a06c330d647eed5591d`
- `compress/zstd_lazy.c` `dd6ccf357165dc8cb574ea56a34ff1db57b7b31728b32103d0c6b72319fd45b1`
- `compress/zstd_lazy.h` `6f13007fbd6824058a73b097960446defe265e33b22c7c6dc2304dcf53998bbf`
- `compress/zstd_ldm.c` `4a2bef3612ae4a9e15a78a55541a152091fa8ba160410db24d495bd6dd57249e`
- `compress/zstd_ldm.h` `bd0cb86041d3a72fd7846e141a41aadae229c8e331a2117df7f6b70a1b4cb08c`
- `compress/zstd_ldm_geartab.h` `7285ac8ba1f0fa57a8cd8158bd427ded4e70e32a58e73b57c420b83a76e5e6e3`
- `compress/zstd_opt.c` `625936ee3fb02d789abb894c1fbc2918ee47582434094c963b630da13532c876`
- `compress/zstd_opt.h` `9b677efbae28909034d3c24f37dc5c4295d267cf477bf04e14653579f90d8b6e`
- `compress/zstd_preSplit.c` `8d7da15de31318ebb7dfe2f2e2c79f05b0d0563dcb92d4525814d05a279d0d64`
- `compress/zstd_preSplit.h` `910facb2e3442e42a6952c514223c4502d95ed957bf7dc73982b3b03785b361b`
- `compress/zstdmt_compress.c` `c83db699b4041bf4db89c5558db490dd295635f1eeefd60b643fbc862bbac7b5`
- `compress/zstdmt_compress.h` `033fb25d96b4295c3d0158edf4d1bd4067f42aac39427706754e6c034b20f80f`
- `decompress/huf_decompress.c` `710a88d877d5dc5c83a4f839392996f337ec81cf38006c4a5be6042de5b54828`
- `decompress/huf_decompress_amd64.S` `7fe53316261517a8f877b793f3bfba8305a3f9ff05c5947f1d709aaff346f1dc`
- `decompress/zstd_ddict.c` `38bf812283d61f4cd1f6aea30f84b43317418257c4e6fc198b09b436bd484397`
- `decompress/zstd_ddict.h` `a97a250fd2f956e3ae419ddde68ae1f9fe4025bc0373254058d31bbd352fa71c`
- `decompress/zstd_decompress.c` `029580818b7e9cd38d9d07c63516b00ceaa943ffcfbd099fc5ccbe3628fa362f`
- `decompress/zstd_decompress_block.c` `9cb8bcb07aeb87e4717a9c8a86d20832dc525dde2c1b72efdaaab7c937f43b87`
- `decompress/zstd_decompress_block.h` `62ee0c6ae3f7353020538397f436b59743da8acb922068841b346f480f7078ac`
- `decompress/zstd_decompress_internal.h` `73217b49a644c0fcf567b70677cae31449ba9f50a235ef3b681001c3f36dc56d`
- `zstd.h` `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b`
- `zstd_errors.h` `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a`

- Licence: BSD-3-Clause, the release's `LICENSE`. The release's
  `COPYING` (GPL v2) is not copied.

The files are unchanged.
