#!/usr/bin/env bash
# Add remote access to the local Holo rootfs (build/steamos/rootfs) before finalize-holo-rootfs.sh:
#  - Wi-Fi without persistent writes (quest1-modem.service, wpa_supplicant, dhcpcd on wlan0)
#  - the GPU through libhybris (quest1-gpu.service, Android HAL daemons, hybris-run)
#  - Monado (OpenXR runtime, monado.service) with the xrgears demo; xr-run starts clients: xr-run xrgears
#  - audio: ADSP boot, CM710X sound card and Android's speaker route (quest1-audio.service), ALSA
#  - SLAM: Basalt (/opt/basalt from tools/build-basalt.sh aarch64) with its runtime libraries:
#    OpenCV (with qt6-base for highgui), oneTBB, fmt, boost-libs and libepoxy (basalt_vio)
#  - iw, ip (iproute2) and hostname (inetutils) for work on the headset
#  - network sockets for non-root (Android "inet" group), so timesyncd sets the clock; networkd on 4.4
#  - OpenSSH: root login with a key only
# Secrets come from SECRETS_DIR (default ~/.local/share/QuestStack/quest1-remote), never the repo:
#   wpa_supplicant-wlan0.conf (Wi-Fi network and password)  root.pub (SSH key for root)
# The resulting holo.img contains the Wi-Fi password: keep it local, never publish it.
# Usage: tools/add-holo-remote.sh [SECRETS_DIR]
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd -P)
W=$P/build/steamos
R=$W/rootfs
S=${1:-$HOME/.local/share/QuestStack/quest1-remote}
REPO=https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3
# repo filename sha256 (from the snapshot's core.db/extra.db). From opencv-4.12.0 on: Basalt's
# runtime closure (opencv qt6-base onetbb hwloc fmt boost-libs libepoxy, minus the base rootfs).
PKGS="core openssh-10.2p1-2-aarch64.pkg.tar.zst b96ab7c370317b95543420e34b544317272d48560d0a559d83de8dc501073713
core libnl-3.11.0-1-aarch64.pkg.tar.zst 2c7d4af1dec8c1f444635c4d890b75a9c0cf035565a5eecbea8d4f01deaba959
extra duktape-2.7.0-7-aarch64.pkg.tar.zst 24807fc3fa8d08db6ae5b8fd262ffcbb957a2380ed6113e5f30dccd5e1ff1935
extra polkit-126-2.2-aarch64.pkg.tar.zst fb172ac3222cfa49538b1afb0433ff7b50a355af4d82c229a40c9cc5fb62689e
extra pcsclite-2.4.0-3-aarch64.pkg.tar.zst 1660db51aa01e87ad8a3530c2b4a3483c8d0fc58c7266979adf26efd1fb3f65c
core wpa_supplicant-2:2.11-3-aarch64.pkg.tar.zst 3201a7af3766be5f15e8899d31a61d4fe30b4133e46b22312e12c8c9d79a213b
core iw-6.17-1-aarch64.pkg.tar.zst f586ce187e398fff8cb8994c0709d2566e65de53e71f0040cb6c29a23958f0a7
extra dhcpcd-10.3.0-1-aarch64.pkg.tar.zst 6e5d59463cec53b7e450d7435ee296c320599c5dfbea7a9e42eee5a9d9142e7c
core libmnl-1.0.5-2-aarch64.pkg.tar.zst fa3c76a8c9072cbf32dcf59245a0cd9a4a2922f00c84a21cb01f7495aa8c190d
core libnftnl-1.3.0-1-aarch64.pkg.tar.zst be0acce068196ce2682c90612120f6e57c2ba93a93b0f86ea7f1d1b5da68d937
core libpcap-1.10.5-3-aarch64.pkg.tar.zst b799454c2162f56acfe630df20ee73c05007d0ccd79825a53a8c261d0b6d0f4b
core libnfnetlink-1.0.2-2-aarch64.pkg.tar.zst eb2836eb830b2a587e293ce89548ba822c14c92cd385ee70666c346e2146c3a4
core libnetfilter_conntrack-1.0.9-2-aarch64.pkg.tar.zst 708988b0ec002283b30d7e948f87c4217a1ced878be8d3fa1659d22290a2f50b
core iptables-1:1.8.11-2-aarch64.pkg.tar.zst 35171376ec63b9f78427d7465da4f7f7d7adc51970233580161a57d97962c476
core libbpf-1.5.1-1-aarch64.pkg.tar.zst 130e09fb57d4b26342829793d62f9b56814d2b8da1e53252fed66c05445e2f17
core iproute2-6.17.0-1-aarch64.pkg.tar.zst b05e855eb60c40f2b353f7e3ab24e24e6093bd7ac80e733c3b579555c8182e55
core inetutils-2.6-1-aarch64.pkg.tar.zst 0f7237debe5e205e942edff6ef76506bdb85d55c03ddcd2e54a362974b4688cf
core pciutils-3.14.0-1-aarch64.pkg.tar.zst 84008b6de6145c5da676e0b05259025c79a6f7deccd1a2b0584ce4c8e6082a8f
core psmisc-23.7-1.2-aarch64.pkg.tar.zst 92ee5919c0d4b72e600f6751c34ece117b058b463f657787ea2f1efb968c7162
extra alsa-topology-conf-1.2.5.1-4-any.pkg.tar.zst a76b237e24973099b3cee9d3c89f4f8b46d9133411f0992bd46a5131e4427a0c
extra alsa-ucm-conf-1.2.14-2-any.pkg.tar.zst fcd43d67a74282e76f56f5f5ca7c1dcfad3d12ddd7efc51353f84ba177e89181
extra alsa-lib-1.2.14-2-aarch64.pkg.tar.zst d5bed06296bd6817135a282fcd6b7ef7e4a719af94814e7466c0c3878ef9d0b1
extra libsamplerate-0.2.2-3-aarch64.pkg.tar.zst 96dcdf4b472889e5d48be8066c594ef886282bff73c90fa510f7dd2bb3e7d039
extra alsa-utils-1.2.14-1-aarch64.pkg.tar.zst f048dd5a4085f819c20b33589a622723c6ac2f9ab88260c5e93c65a4f4893bf5
extra abseil-cpp-20250814.1-1-aarch64.pkg.tar.zst 95d5afae5b292eb93de1128fc0a5c250fdc5ed71758ce48e294dd6c868138eb1
extra aom-3.13.1-1-aarch64.pkg.tar.zst c0346de420afb4036b8bda7aab4d07f0cb753b2403fecf24bcb78ecc2c03d256
extra avahi-1:0.9rc2-1-aarch64.pkg.tar.zst ada1f3f79978ad0e7a6ffb290e5aecc7dcf9c4c10b02d6523f81787b24f270d9
extra blas-3.12.1-2-aarch64.pkg.tar.zst 4aa9e9e935db768614690cceb3d7565ec670195687ff95dd0eea1571e435f5ae
extra boost-libs-1.89.0-3-aarch64.pkg.tar.zst ede5f59d40ec7aa4a7c9625261ba6ae32b4222c47ab9920668506aa1ec9ac180
extra bubblewrap-0.11.0-1-aarch64.pkg.tar.zst e654a794dc3ab778cb060f7413ef9460e069d4229654ac7df152d3a598a29e2a
extra cairo-1.18.4-1-aarch64.pkg.tar.zst d3d64a92e050a35877154ae55391221b999f300fc8fecda5ff012c4bc8ab5a7e
extra cblas-3.12.1-2-aarch64.pkg.tar.zst db0df7eb2d3401c0125e777bdedc1a4d782bf242b11ba4e5c5fde90f4e539f5f
extra cdparanoia-10.2-9-aarch64.pkg.tar.zst 5bada55cd1b565de753cfc126a8c8d64d138f91ef96177c1cd9802dbc0d01f30
extra dav1d-1.5.2-1-aarch64.pkg.tar.zst 7ff03e96707f7be022b88dac8501556dccd91686d39f5eb960016c2deb00488b
extra double-conversion-3.3.1-1.2-aarch64.pkg.tar.zst a4e5d9e7577cde2f90fd74d609547b6993ac46981a97fe40051331c503b062ee
extra ffmpeg-2:8.0-3-aarch64.pkg.tar.zst 9be2ecaa588c389bf3847d4275d3f7435f6365a9b23b8aed32276717e871c615
extra fftw-3.3.10-7-aarch64.pkg.tar.zst 0c8837554f2901418d9fa745905253edb9e6da7d6031e8c163d476dc4ebd63a5
extra flac-1.5.0-1-aarch64.pkg.tar.zst 8864a9348dd3c079e89b62f9c578be900ff63579605ba83de6e6dd0dc78706f2
extra fmt-12.1.0-1-aarch64.pkg.tar.zst a756a84a5ecdd9a1d545d32c304d60304eb98f49cf9775ae7b788dccf0e5612a
extra fontconfig-2:2.17.1-1-aarch64.pkg.tar.zst 10c55d9d0abca7ea40d778b34cb2fce5f39974042f4f6681c4c541fa24dc771f
extra freetype2-2.14.1-1-aarch64.pkg.tar.zst 41e077053d6227d02217b609dc5dd8d3c478df2148ff6c9ffac7546645615c7f
extra fribidi-1.0.16-2-aarch64.pkg.tar.zst 111a98b837dc6b5d9008b4c3a19bc5baffbcb1096625d758aeb1d51a6e507e3d
extra gdk-pixbuf2-2.44.4-1-aarch64.pkg.tar.zst b02cb85da7c997dc976d0e58b97d878cdf85b7e246c940e09749f5fa1a5cb164
extra giflib-5.2.2-2-aarch64.pkg.tar.zst d097293bb54b251d309f4061164b7fe698bd5e8549ac34bec538ad2fa2cd3fa8
extra glycin-2.0.7-1-aarch64.pkg.tar.zst a68d2e0fcfd4641ec8c19163c3b735650adb434c480038b4545cbe867908c408
extra gperftools-2.17.2-1-aarch64.pkg.tar.zst 56257180c947aafe4599da2c8ff97fc02e32057a11e0b9a03f5c50d85f92f6e3
extra graphene-1.10.8-2-aarch64.pkg.tar.zst 92711ba8804cd11631219a0efa64cbe658ea42013607a1235b7160f84ed78b2a
extra graphite-1:1.3.14-5-aarch64.pkg.tar.zst 5fd8471f8c946c2f8b5d21896e640c66b8842e9484f79d7b09963f728b749c23
extra gsm-1.0.22-2-aarch64.pkg.tar.zst c384132ba71fbf59c237b5436aa5efe209414ba8ba8f961f78171ce8fd7b342a
extra gst-plugins-base-1.26.8-1-aarch64.pkg.tar.zst 5aedd7db8c57ea712e7938cd97815849ec5de513d7e90f1fb0f81e4af2b948c6
extra gst-plugins-base-libs-1.26.8-1-aarch64.pkg.tar.zst 0527f32869a285cbcac7279e85ee853daca3818e44e2cbbf68a0084376314644
extra gstreamer-1.26.8-1-aarch64.pkg.tar.zst 4bf90a1ab8e22d136e7bb19b68a70fdcb610d902321ffd6c5a411f7d7ecd061a
extra gtest-1.17.0-1-aarch64.pkg.tar.zst b21385658d601c17ff55c389b1b3c91bd3e80dcbd73b00dec8f8ef4847b37b6c
extra harfbuzz-12.2.0-2-aarch64.pkg.tar.zst 2b4d41a064abd5fc0c3fd1835811367f1304243bfea173277ecf6dc63b4e5c62
extra highway-1.3.0-1-aarch64.pkg.tar.zst 25ed329d20c0e63eb152ec16f4305e2eb1c21df9688518885d2dacffe8752319
extra hwloc-2.12.2-1-aarch64.pkg.tar.zst f811b5b7ff4c86e0353427b0463982041ddc26f75c7b4bd8e05cd89db6a74199
extra imath-3.2.2-2-aarch64.pkg.tar.zst 34d0792c932fca7c0ae55120bccd62ec08029b4ddd864ede03f547a6d179c4f1
extra iso-codes-4.19.0-1-any.pkg.tar.zst f58c9265d83cd53ec0071545fb275ad273bc4d014c973a35c4fca8c2224994c6
extra jack2-1.9.22-1-aarch64.pkg.tar.zst 53d1dd5d7b192204289ddb74a077046892cd31ae02156fc77ee635ef412242c4
extra jbigkit-2.1-8.2-aarch64.pkg.tar.zst 7ab8636db9aead92eee46123469adbd2c72775a45af6aa7a6fc766b36b83e319
extra lame-3.100-6-aarch64.pkg.tar.zst db94be227bc3471df34fd9f0c53590750535304a0a232dd6ab7f86178bb56d1a
extra lapack-3.12.1-2-aarch64.pkg.tar.zst 69f552f8254589550e4fb0215a30b975cb9f7827d07f4577163ba36c709dfe6b
extra lcms2-2.17-1-aarch64.pkg.tar.zst 5eba0d94a02c830c9639473708e7ac5f2e069dd81b52e7f5c11f3c3b4d9c5375
extra libass-0.17.4-1-aarch64.pkg.tar.zst 71ba7f50bdc4fc7573144bd40b2eab7c75f94c9724dcb0893548c33aada66b8c
extra libasyncns-1:0.8+r3+g68cd5af-3-aarch64.pkg.tar.zst 74d751c640903200413a6f04c633ddcb3195b8024efd40d9927d486e61f64419
extra libavc1394-0.5.4-7-aarch64.pkg.tar.zst 7ef4d28428d403c9d6691b6c843357e76d60a99b1b5e0f5d2777bc2db5d31d08
extra libb2-0.98.1-3-aarch64.pkg.tar.zst 4899fec2cf40f5c08bc838941e633e4946e17eb9d8ca5bd65344ca36b1665d4d
extra libbluray-1.4.0-2-aarch64.pkg.tar.zst f9b917ebd4a4295afec0e187c983f268ba91188eb7b7663df54241321829a66a
extra libbs2b-3.1.0-9-aarch64.pkg.tar.zst f8c4e8275d4f72b7da962c3045b167c4d6c4d7e61f01ea7a3ce0c0125c21d5ec
extra libcups-2:2.4.14-1-aarch64.pkg.tar.zst 9213aaa189c79e534f014a134b675b18948502ded084542d496d5b0e5a7981f1
extra libdaemon-0.14-6-aarch64.pkg.tar.zst 2b87acae2cfa1ee3bc1f54304ff88bbbf860dc3c0a5a17d7a6cb721593187fc7
extra libdatrie-0.2.13-4.2-aarch64.pkg.tar.zst cbdca76c190f294636a1814e66663065e1d72a695a3934552ca9278f26968490
extra libdc1394-2.2.7-1.2-aarch64.pkg.tar.zst fdf81f4c820af0337c69c71d91ac6c5e89fa8578c2b8b80aa68a4a89aff66ad1
extra libdeflate-1.24-1-aarch64.pkg.tar.zst 3f9d7d6e88b00486f91d46acaba0c06205c1cc6e065fc844084431f3f1a873a8
extra libdovi-3.3.2-1-aarch64.pkg.tar.zst a19cec2ef84ae87701537b62cf08bd39bec26d46b6866fa8f406fe464e18d5d4
extra libdvdnav-7.0.0-1-aarch64.pkg.tar.zst 0a4631272fa3118dc8dd6e708e0afcf1bdec1ddab2b6092fa0a02d105fcf7f06
extra libdvdread-7.0.1-1-aarch64.pkg.tar.zst 1e69e1cb1fd6e441106dd3276b89f757054738415dd90d7ba61fbab1e45a8323
extra libepoxy-1.5.10-3-aarch64.pkg.tar.zst e31b35835f495a2cfbc3ffeb001793a56e2cca3b8e2051d00654fb0376916461
extra libevdev-1.13.5-1-aarch64.pkg.tar.zst 166a147c63b838f7b133140ef25982ccbcefb9bf4d9a3b853ca86c15b3824013
extra libgudev-238-3-aarch64.pkg.tar.zst 97bb9ff56b0f600c7083a57ea6579491c1a9d612a48ffb92e68e7420524c5d58
extra libice-1.1.2-1-aarch64.pkg.tar.zst 022758604c9357e31116fe22841cf05b1984d1b3e1baaa57b0a56fc1ce46568c
extra libiec61883-1.2.0-9-aarch64.pkg.tar.zst 4e932d117e897d40cda965b30692773c6fe96bb0f050eb2cadb384e63c69b3a7
extra libinput-1.29.2-1-aarch64.pkg.tar.zst 845d4340e56d2004c96da6f1661e8ec36e31bbde0e2f9f4375a55a55a6d02216
extra libjxl-0.11.1-5-aarch64.pkg.tar.zst 4116e3f9726f23cc1c5af3dd6c48b0120016ca9ccb8230ddf48ff8791c9a9196
extra libmodplug-0.8.9.0-6-aarch64.pkg.tar.zst 62eee1fa73ff455856494e74d0064bf3cc7c4af1b6fc90775be747ef678ed60b
extra libogg-1.3.6-1-aarch64.pkg.tar.zst 5b9be187865e0aa0fedaa790ad50f08a47d21475e84576f6d524be7b6092dad8
extra libopenmpt-0.8.3-1-aarch64.pkg.tar.zst 1455032a194aa302b708d8f8f357240e06c6879964b96ad89ee65a4b4ffde1d5
extra libpgm-5.3.128-3.2-aarch64.pkg.tar.zst 68589e1342b6fd3fce294ac6f29645988556e0be37b4da39d5e4dc2adfe1d45c
extra libplacebo-7.351.0-3-aarch64.pkg.tar.zst 8c1c11fa03e13aca52bdc164cc774319b052949b5476cc8e7150f072d597a290
extra libproxy-0.5.11-1-aarch64.pkg.tar.zst 099ae70c1a88104158ea98bf4797f0b687d0c96c886b75e27cb9d0fd61fb22aa
extra libpulse-17.0+r93+gbe2efb69b-1-aarch64.pkg.tar.zst c854fb0c9ab5e9c767b65a9dcb253a3ac1f4fc0d2488d9b8d01630deba847dc4
extra libraw1394-2.1.2-4-aarch64.pkg.tar.zst d3f94c4db89449eee0b1326d115cc1635c3935df7294ca539d4712750a2cd0ff
extra librsvg-2:2.61.3-1-aarch64.pkg.tar.zst f46245dcc1b7c7a3c3546b576d6ab2a79f1f275924c36d3329cb5b4323a5c3a8
extra libsm-1.2.6-1-aarch64.pkg.tar.zst c9e23265d839a78c10d9922018204fbea83fc74b41d9d2d47ab77d4e6af82b49
extra libsndfile-1.2.2-4-aarch64.pkg.tar.zst d7f9faba3480054425f1037f4599de2a5ace195ce68f47b41b82dc8b2ee53f09
extra libsodium-1.0.20-1-aarch64.pkg.tar.zst 103ad5de31ea3a84e72fc16a733fd395f85b9b3f55e6f678e0d684039cef924e
extra libsoxr-0.1.3-4-aarch64.pkg.tar.zst dd1a0f23fdf2cbc6d6932c9e3ddc61b426caf0c9089236c41b7169303e502132
extra libssh-0.11.3-1-aarch64.pkg.tar.zst 8b887533e226a339508f1cf3789a671194fe55d8fb7f021e5c61ba8e7b3684f7
extra libthai-0.1.29-3.2-aarch64.pkg.tar.zst c8edd3a8b26de80fe25b1d24568ca2d2d025fa9ec95d6db369cfa029b16629c5
extra libtheora-1.2.0-1-aarch64.pkg.tar.zst 853696c06287637750441b5b7091c5a283e0a582ddac2ba25a1bf1a81b5c6a5d
extra libtiff-4.7.1-1-aarch64.pkg.tar.zst b5b7c13e76b6ea63411c69dfb83e5f42bff92b8f8cf082a2986caca9f0eea680
extra libunibreak-6.1-1-aarch64.pkg.tar.zst 952dc56d599b503923ecfe5292bd7d081527848eb8133f7a10fb83ab48a315f4
extra libva-2.22.0-1-aarch64.pkg.tar.zst 2ce788ae338bdc2fe3a5f0d7e8306664e1e7498545c351a972e8e6e577f13659
extra libvdpau-1.5-3.2-aarch64.pkg.tar.zst c460b32b997675411c077376093d156c7dab01dbe32845c1ca47d5f9e8ebaa04
extra libvorbis-1.3.7-4-aarch64.pkg.tar.zst 5c19f0d88c80177800e859bc7d079a6e136b1621adea2da18f5a31eed2c0c5e7
extra libvpl-2.15.0-1-aarch64.pkg.tar.zst 11ea684d2c270ade079584dfa4ea9010d4904e3f56e7d6d3d4ada00b27e08f9c
extra libvpx-1.15.2-2-aarch64.pkg.tar.zst af11ef4f60cb8e1c421e9d41ab54b3674517fa54f20735c55f7ce70da2de550d
extra libwacom-2.17.0-1-aarch64.pkg.tar.zst f30c332d57bb3012498dadd0b4e5bc37e70b632ec541603ea917d4b3dbfb2fb8
extra libwebp-1.6.0-2-aarch64.pkg.tar.zst d9d6e55d480c5688dd5b05530b25b5f59f877814d78231e6277f153c34834c43
extra libxft-2.3.9-1-aarch64.pkg.tar.zst 4f5743f0c6e7f0ab9636b21992e09ac1dd0a46a43c15a5aa082619f024c24e6d
extra libxkbcommon-x11-1.13.0-1-aarch64.pkg.tar.zst fecbc48558a2632655baf9b4b0bf6dcf1dddca67fd8dd477a77f47e61e2984bb
extra libxmu-1.2.1-1-aarch64.pkg.tar.zst b01564bbc6e6755e172a7d4e050ee59e38b51c405c8197c03af3eea221c65a1a
extra libxt-1.3.1-1-aarch64.pkg.tar.zst e42b25f49f8a453512da92a9c0d6abaf9633f0c1743a93476af775b926787214
extra libxv-1.0.13-1-aarch64.pkg.tar.zst 0e4a8823cb69d1939bc496f7a7cabf560ba807a98fbe67ddda97e742b4cee5d1
extra l-smash-2.14.5-4-aarch64.pkg.tar.zst fda53ac5847da984337fbf0fda25e9e0487bbf66573a48a870355166905e8f8a
core lzo-2.10-5-aarch64.pkg.tar.zst ff11d9a5060ae28fbece3faad126724314232747cc09b836157f02ee09d3869a
extra md4c-0.5.2-1-aarch64.pkg.tar.zst b542000427e8b628fc344dcc0a00bab5d1b4fb470d046591cf3702cc96283d85
extra mpg123-1.33.3-1-aarch64.pkg.tar.zst b4f6497423537bdc11179bc9927bf3014bcf960eb78936eb108a9bbbdab7bbba
extra mtdev-1.1.7-1-aarch64.pkg.tar.zst ddc6848de75a95048baacb5a27f06980dc1f8384f41bd5346d0dd048ee8ebd86
extra ocl-icd-2.3.4-1-aarch64.pkg.tar.zst 9c7d001bdae2d19a4fc2c050a1a14bbb17853989880824964095a4b85ad14374
extra onetbb-2022.3.0-2-aarch64.pkg.tar.zst f08ac1cad31d7f12c25ab979e214ff22c1b370b7656a29d1b2fd3c011e6cb063
extra opencore-amr-0.1.6-2-aarch64.pkg.tar.zst 4fa7055c225c274bd2b76c617dd48ade52ea0f1316320eeff49411e652e52cbf
extra opencv-4.12.0-13-aarch64.pkg.tar.zst 94e775dfb72fa050cb237389ea46a7f7a06ed77bb5b3d304b00ab177a54e8cda
extra openexr-3.4.3-1-aarch64.pkg.tar.zst ef00433eeacb666d4f4d6af801aeb3351edc411d8960b94eb2a6a63c91819efa
extra openjpeg2-2.5.4-1-aarch64.pkg.tar.zst 1f5d24b1f8ae967010f553244cd905f2d06e58003f486459c90a654fd4047b90
extra opus-1.5.2-1-aarch64.pkg.tar.zst 34f280315ceccde55eadbe6d423be541b63af1e932fc7c2bfb7e7c6a8f9a68c7
extra orc-0.4.41-1-aarch64.pkg.tar.zst 2ff0aec69782078473ea132086957bda3e7773565adee5762ce5271edf4d3a7b
extra pango-1:1.57.0-2-aarch64.pkg.tar.zst 2c747bc0f8e09206376d2af888e044a036a6370d6f2d06de33066f887da54e99
extra pixman-0.46.4-1-aarch64.pkg.tar.zst b2133d3630f753b60d09140d5ba500f82e7bbcba7605b90c027a9900d57be6f7
extra portaudio-1:19.7.0-3-aarch64.pkg.tar.zst 069b4dda9a6411f4013ef2f4254e2f931ecc4082bf08f7d5f34693d620eb1269
extra protobuf-33.0-2-aarch64.pkg.tar.zst 5ec60853229827998d4d0ff096d76a635d9ec65a4346dedf01aae480286de405
extra qt6-base-6.10.0-3-aarch64.pkg.tar.zst fd2a8e2e647d98948e484010a03d7c834dd1c841deafe109400b303af51d6dac
extra qt6-translations-6.10.0-1-any.pkg.tar.zst fa1e9dbefb114a776582fd29ba2aa91ba60a5a550ba25174eb69af2189e944e0
extra rav1e-0.8.1-2-aarch64.pkg.tar.zst 23c38edb528a8a0622190d7a6c97151bd3cc9bc1fdf1bf80a36b7d42748599f3
extra rubberband-4.0.0-1-aarch64.pkg.tar.zst 024e25e398b94884d060b85877ba4a7068a40c4b0cfc8d3346c2ca64debb9ed9
extra sdl2-compat-2.32.58-1.2-aarch64.pkg.tar.zst 8b3d4429706db6bd1a4f6c120a15b8c178e1b25b072aa329ba56f74d73354f4b
extra sdl3-3.2.26-1-aarch64.pkg.tar.zst 575ba9be1c53fb7551fe559bd259323d1b3cc27f2ed231114a4fa66e1ed10ad4
extra shared-mime-info-2.4-2-aarch64.pkg.tar.zst a182bece2fad7340a3753baa669ecf98a44f4acf02f923a96504b2fec4cd40bd
extra snappy-1.2.2-2.2-aarch64.pkg.tar.zst 890811c7044d501ca465f5860416ad71faa64e09881f2ed06759572053268bdb
extra speex-1.2.1-2-aarch64.pkg.tar.zst c3fa7725e87b87379b958ef48a236b368ae31a5832f2aca4c7be7c8b81d458a3
extra speexdsp-1.2.1-2-aarch64.pkg.tar.zst 8cb5038f7959d43a56ce03db8fee40b25068038633442163acc07a46f811e37e
extra srt-1.5.4-1-aarch64.pkg.tar.zst 397e6fb68cafe0d7d0b53e9fc5f776bac686fb1342d0041dd458e001e057d9dc
extra svt-av1-3.1.2-1-aarch64.pkg.tar.zst 24a2530585634d2662d0ecb5c49dc17a1e7f62b002e146480d5b29288ffa3113
extra tslib-1.24-1-aarch64.pkg.tar.zst 37328751a16d9f6dabdca03a67f5b647d9fd45b8c2329eb379543ac068cff976
extra v4l-utils-1.32.0-1-aarch64.pkg.tar.zst d24e5276f991e88ea07937635b85c38269a4a5824422b05daf2e72a96b30285d
extra vapoursynth-72-1-aarch64.pkg.tar.zst 4b3ae4c33c8078bbd40550d863d55ddae6ab048e2124593d353730863ad9f5e2
extra verdict-1.4.4-1-aarch64.pkg.tar.zst dfb7f31d263d41e517fb44e90611a3e6ba65bbad76b222e6d7ef5810f621d45b
extra vid.stab-1.1.1-2-aarch64.pkg.tar.zst 076251c66dc2a7fd8d6be73b9727f083ff6c222f6858e6e824c721915b563182
extra vmaf-3.0.0-1-aarch64.pkg.tar.zst 1dc3d936976186de63297344c6a827d4d33c0aa17b6c0c40d63fc095fe83b53f
extra x264-3:0.165.r3222.b35605a-2-aarch64.pkg.tar.zst ba86bd8910bb3703d9138fd3e60f2b083484283b0a49ba45ac815743088b679a
extra x265-4.1-1-aarch64.pkg.tar.zst d4d3d1c133af9680bd19b1ad6d2b0d5138e5b494998faa7df94b4c9fb61bdf3a
extra xcb-util-cursor-0.1.6-1-aarch64.pkg.tar.zst c03222145d8fb2fdd28a7304259165a7eba7ca0630ab603a478da23245e21763
extra xcb-util-keysyms-0.4.1-5-aarch64.pkg.tar.zst acb7990422b639254cef13828d6ccd7b7e524c6a3caac2cbace9e0937ac206ca
extra xcb-util-renderutil-0.3.10-2-aarch64.pkg.tar.zst e6601ae6c355570d1f4decaaf1386139da812c2eaa37edf40090f4a06f7af403
extra xcb-util-wm-0.4.2-2-aarch64.pkg.tar.zst a8d3059b374c8994eeaceb9c2ef42e9a9156653f974b018da359e3b5c91fe47a
extra xdg-utils-1.2.1-1-any.pkg.tar.zst 5d03308a9b9dd8cc8375c087c4b91156de0059a7cb9675f69b55e895b8ecb79f
extra xorg-xprop-1.2.8-1-aarch64.pkg.tar.zst bdf2eb4cc1bf9650bd4404557e66562e9dfa644d612a12470c46c9b595efdc8a
extra xorg-xset-1.2.5-2-aarch64.pkg.tar.zst 16a262387aa4f8dd1d6eb0e6d49bb0c562caf0a14a581955d6010fa84c2c04ae
extra xvidcore-1.3.7-3-aarch64.pkg.tar.zst dcbb8e038b145458f5eacfbf97018ec89681c4525379ade15c5e7e0e933649e9
extra zeromq-4.3.5-2-aarch64.pkg.tar.zst f4ff054de28f131f48c9440316da94359ecf9616e7f0dce01ca0bfbd1084464a
extra zimg-3.0.5-1-aarch64.pkg.tar.zst 4912d142c143606d8767f047d2b87c1afca2cadbb026632423274a97c883b3d1"

[[ -f $R/etc/quest1-base-release ]] || { echo "No prepared rootfs at $R" >&2; exit 1; }
B=$P/build/basalt-aarch64/install/opt/basalt
[[ -f $B/lib/libbasalt.so.2 ]] || { echo "No Basalt in $B: run tools/build-basalt.sh aarch64" >&2; exit 1; }
for f in wpa_supplicant-wlan0.conf root.pub; do
	[[ -s $S/$f ]] || { echo "Missing secret $S/$f" >&2; exit 1; }
done
for k in root.pub; do
	grep -qE '^(ssh-|ecdsa-)' "$S/$k" || { echo "Not a public key: $S/$k" >&2; exit 1; }
done

# 1. Packages: download, verify against the snapshot's database hashes, install in the rootfs
D=$W/downloads/packages
I=$W/sources/quest1-remote-pkgs
mkdir -p "$D" && rm -rf "$I" && mkdir -p "$I"
files=()
while read -r repo file sha; do
	[[ -f $D/$file ]] || curl -fsSL -o "$D/$file" "$REPO/$repo/os/aarch64/${file//:/%3A}"
	echo "$sha  $D/$file" | sha256sum -c --quiet
	cp "$D/$file" "$I/"
	files+=("/build-sources/quest1-remote-pkgs/$file")
done <<< "$PKGS"
exec 9>/tmp/cxx-build.lock
flock 9
"$P/tools/run-holo-local.sh" /usr/bin/pacman -U --needed --noconfirm "${files[@]}"
"$P/tools/run-holo-local.sh" /bin/sh -c '
	# CONFIG_ANDROID_PARANOID_NETWORK: only root and GID 3003 (AID_INET) may open IPv4/IPv6
	# sockets; anyone else gets EACCES ("Temporary failure in name resolution").
	getent group inet >/dev/null || groupadd -r -g 3003 inet
	usermod -aG inet alpm  # pacman DownloadUser, should it be enabled'
rm -rf "$I"

# 2. Files. pack-holo-rootfs.py makes everything not owned by a package root:root, modes kept.
put() { install -D -m "$1" "$2" "$R/$3"; }
link() { mkdir -p "$(dirname "$R/$1")"; ln -sfn "$2" "$R/$1"; }
unit() { printf '%s\n' "$2" > "$R/etc/systemd/system/$1"; chmod 644 "$R/etc/systemd/system/$1"; }

put 755 "$P/device/quest1-modem.sh" usr/local/sbin/quest1-modem
put 644 "$P/tools/mkproparea.py" usr/local/lib/quest1/mkproparea.py
mkdir -p "$R/android" "$R/system" "$R/firmware" "$R/apex" "$R/persist" "$R/mnt/vendor/persist" "$R/data"
link vendor /system/vendor
unit quest1-modem.service '[Unit]
Description=Modem subsystem and WCN3990 Wi-Fi firmware (no persistent writes)
# Never stopped from Linux: closing /dev/subsys_modem shuts the modem down and the next load hangs
# the SoC. Not part of the shutdown transaction (no Conflicts=shutdown.target), and quest1-modem
# marks its cgroup so that systemd-shutdown spares the daemons until the final reboot().
DefaultDependencies=no
After=local-fs.target sysinit.target
IgnoreOnIsolate=yes
# Only tries user.* (rejected by kernel 4.4), but keeps systemd from removing the trusted.* xattr
SurviveFinalKillSignal=yes

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-modem
TimeoutStartSec=120

[Install]
WantedBy=multi-user.target'
link etc/systemd/system/multi-user.target.wants/quest1-modem.service /etc/systemd/system/quest1-modem.service

# GPU: Meta's Adreno Vulkan driver through libhybris (/opt/hybris, from the base build). It needs
# the HAL overlay (quest1-gpu) and three Android daemons from system_b: the HIDL registry, the
# graphics allocator (gralloc buffers) and configstore (libvulkan waits forever without it).
[[ -f $R/opt/hybris/lib/libbionictls.so ]] || { echo "No libhybris in $R/opt/hybris" >&2; exit 1; }
put 755 "$P/device/quest1-gpu.sh" usr/local/sbin/quest1-gpu
put 755 "$P/device/hybris-run.sh" usr/local/bin/hybris-run
unit quest1-gpu.service '[Unit]
Description=Adreno Vulkan and gralloc as the libhybris HAL defaults
# quest1-modem mounts system_b; it may still fail later, on the Wi-Fi
Wants=quest1-modem.service
After=quest1-modem.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-gpu'
AENV='Environment=ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime'
unit android-hwservicemanager.service "[Unit]
Description=Android hwservicemanager (HIDL registry)
Requires=quest1-gpu.service
After=quest1-gpu.service

[Service]
$AENV
ExecStart=/system/bin/hwservicemanager
Restart=on-failure

[Install]
WantedBy=multi-user.target"
hal() { # name description binary
	unit "android-$1.service" "[Unit]
Description=Android HAL: $2
Requires=android-hwservicemanager.service
After=android-hwservicemanager.service

[Service]
$AENV
# hwservicemanager.ready is preset in the property area: give the registry time to start
ExecStartPre=/bin/sleep 0.5
ExecStart=$3
Restart=on-failure
RestartSec=1

[Install]
WantedBy=multi-user.target"
	link "etc/systemd/system/multi-user.target.wants/android-$1.service" "/etc/systemd/system/android-$1.service"
}
link etc/systemd/system/multi-user.target.wants/android-hwservicemanager.service /etc/systemd/system/android-hwservicemanager.service
hal gralloc "graphics allocator" /system/vendor/bin/hw/android.hardware.graphics.allocator@2.0-service
hal configstore configstore /system/vendor/bin/hw/android.hardware.configstore@1.1-service

# wpa_supplicant-nl80211@wlan0 reads wpa_supplicant-nl80211-wlan0.conf
put 600 "$S/wpa_supplicant-wlan0.conf" etc/wpa_supplicant/wpa_supplicant-nl80211-wlan0.conf
rm -f "$R/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
# Started when the driver creates wlan0
link etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/wpa_supplicant-nl80211@wlan0.service /usr/lib/systemd/system/wpa_supplicant-nl80211@.service
link etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/dhcpcd@wlan0.service /usr/lib/systemd/system/dhcpcd@.service

# SSH: keys only, root with the key from root.pub.
put 644 /dev/stdin etc/ssh/sshd_config.d/10-quest1.conf <<'EOF'
PermitRootLogin prohibit-password
PasswordAuthentication no
KbdInteractiveAuthentication no
EOF
link etc/systemd/system/multi-user.target.wants/sshd.service /usr/lib/systemd/system/sshd.service
put 600 "$S/root.pub" root/.ssh/authorized_keys
chmod 700 "$R/root/.ssh"

# Time: timesyncd runs as systemd-timesync and needs inet (see above) to resolve and reach NTP
# servers. Without it the clock stays wrong (the RTC reads 1970) and TLS fails, e.g. in pacman.
put 644 /dev/stdin etc/systemd/system/systemd-timesyncd.service.d/10-android-inet.conf <<'EOF'
[Service]
SupplementaryGroups=inet
EOF
# networkd: kernel 4.4 has no nexthop objects; enumerating them fails fatally
# ("Could not enumerate nexthops: Operation not supported").
put 644 /dev/stdin etc/systemd/networkd.conf.d/10-quest1-kernel-4.4.conf <<'EOF'
[Network]
ManageForeignNextHops=no
EOF
# usb0: keep the initramfs address even when networkd stops; Wi-Fi (dhcpcd) decides "online",
# so a missing USB cable doesn't stop timesyncd.
n=$R/etc/systemd/network/10-usb0.network
grep -q '^KeepConfiguration=' "$n" || sed -i 's/^ConfigureWithoutCarrier=yes$/&\nKeepConfiguration=yes/' "$n"
grep -q '^RequiredForOnline=' "$n" || sed -i 's/^\[DHCPServer\]$/[Link]\nRequiredForOnline=no\n&/' "$n"
# Kernel 4.4 lacks what networkd needs for IPv6 router discovery and its DHCP server (both fail
# with "Protocol not available" and put usb0 in the failed state). The initramfs keeps busybox
# udhcpd running for the host instead.
sed -i 's/^DHCPServer=yes$/DHCPServer=no/' "$n"
grep -q '^IPv6AcceptRA=' "$n" || sed -i 's/^KeepConfiguration=yes$/&\nIPv6AcceptRA=no/' "$n"

# Shutdown: without kernel/cgroup-events-frozen.patch systemd never notices that a cgroup became
# empty, so every stop that waits for leftover processes runs into its timeout twice. Short
# timeouts bound that; harmless with the patch.
put 644 /dev/stdin etc/systemd/system.conf.d/90-quest1-shutdown.conf <<'EOF'
[Manager]
DefaultTimeoutStopSec=10s
EOF
put 644 /dev/stdin etc/systemd/system/user@.service.d/90-quest1-shutdown.conf <<'EOF'
[Service]
TimeoutStopSec=10s
EOF
# dhcpcd -x (ExecStop) and then SIGTERM each hang for the full timeout at shutdown, once wlan0 is
# going away; nothing needs the lease released.
put 644 /dev/stdin etc/systemd/system/dhcpcd@wlan0.service.d/90-quest1-shutdown.conf <<'EOF'
[Service]
ExecStop=
TimeoutStopSec=3s
EOF
# udev's v4l_id hangs for minutes on the msm camera nodes (workers killed after 3 min), and its open
# of the Venus nodes starts a firmware load before /run/fw exists. Nothing needs v4l names.
link etc/udev/rules.d/60-persistent-v4l.rules /dev/null

# OpenXR: Monado (/opt/monado, built with the Quest 1 driver) as a service; clients run through
# xr-run, e.g. xr-run xrgears. The proximity sensor blanks the display as usual (no QUEST1_IGNORE_PROX).
[[ -x $R/opt/monado/bin/monado-service ]] || { echo "No Monado in $R/opt/monado" >&2; exit 1; }
put 755 "$P/device/xr-run.sh" usr/local/bin/xr-run
unit monado.service "[Unit]
Description=Monado OpenXR runtime
Requires=quest1-gpu.service android-gralloc.service android-configstore.service
After=quest1-gpu.service android-gralloc.service android-configstore.service

[Service]
RuntimeDirectory=monado
Environment=XDG_RUNTIME_DIR=/run/monado HOME=/root XRT_NO_STDIN=1 XRT_COMPOSITOR_SCALE_PERCENTAGE=85 XRT_COMPOSITOR_COMPUTE=0
ExecStart=/usr/local/bin/hybris-run /opt/monado/bin/monado-service
Restart=on-failure

[Install]
WantedBy=multi-user.target"
link etc/systemd/system/multi-user.target.wants/monado.service /etc/systemd/system/monado.service

# Audio: the ADSP is booted once per boot (never unloaded); the CM710X codec firmware and the
# mixer settings come from system_b, so this needs quest1-modem's mounts and firmware path.
put 755 "$P/device/quest1-audio.sh" usr/local/sbin/quest1-audio
put 644 "$P/device/mixer-paths.py" usr/local/lib/quest1/mixer-paths.py
unit quest1-audio.service '[Unit]
Description=ADSP, CM710X sound card and the speaker route
# quest1-modem may still fail later, on the Wi-Fi
Wants=quest1-modem.service
After=quest1-modem.service
Before=sound.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-audio

[Install]
WantedBy=multi-user.target'
link etc/systemd/system/multi-user.target.wants/quest1-audio.service /etc/systemd/system/quest1-audio.service

# SLAM: Basalt for Monado's SLAM tracker (VIT, libbasalt.so) and its tools (basalt_vio etc., run
# with --show-gui 0); RUNPATH /opt/basalt/lib.
rm -rf "$R/opt/basalt"
cp -a "$B" "$R/opt/basalt"

f=$R/etc/quest1-base-release
grep -q '^REMOTE_ACCESS=' "$f" || cat >> "$f" <<'EOF'
REMOTE_ACCESS=wifi+ssh (image contains the Wi-Fi password: do not publish)
EOF
echo "Remote access added to $R. Next: tools/finalize-holo-rootfs.sh build/steamos/images/<name>/holo.img"
