# Changelog

Notable LunarNX changes are recorded here by the automated release workflow.

## [0.3.0](https://github.com/thinkzhou/LunarNX/compare/v0.2.0...v0.3.0) (2026-09-02)


### Features

* **ci:** automate versioned GitHub releases ([2b609dc](https://github.com/thinkzhou/LunarNX/commit/2b609dc6ef616b9ce23c5aae26931f9a8293bea0))
* **dev-bridge:** name downloaded builds by version ([cfa635c](https://github.com/thinkzhou/LunarNX/commit/cfa635cbf1cb91532936bf9f15bd05bb000546ec))
* **ps:** clarify local pairing ID formats ([d4bf7ff](https://github.com/thinkzhou/LunarNX/commit/d4bf7ff2ff72b3001bde07069a2174b27baeb32d))
* **ps:** harden connection lifecycle and diagnostics ([0644691](https://github.com/thinkzhou/LunarNX/commit/0644691d02eb8dc0b48cd2132d0f6bd9d6a41f02))
* **ps:** harden connection lifecycle diagnostics ([b3a64d8](https://github.com/thinkzhou/LunarNX/commit/b3a64d8e227787c5829e79ff654061bdb0c25d1c))
* **ps:** simplify local pairing account input ([79f0ff5](https://github.com/thinkzhou/LunarNX/commit/79f0ff5d2b0368f9309e198f1f685ac185e91579))
* **ps:** support offline account ID formats for local pairing ([ee0ae56](https://github.com/thinkzhou/LunarNX/commit/ee0ae56bc1752c1bbc601db965de1eb468c1d761))
* **ps:** support phone-assisted local pairing ([84bcc02](https://github.com/thinkzhou/LunarNX/commit/84bcc021cfb9a861d06b248b65399ad341db6afe))
* **ps:** unify local and remote connection planning ([e934dc4](https://github.com/thinkzhou/LunarNX/commit/e934dc4bfeba3ee6daf8b04602fc835287f10e1b))
* **stream:** show disconnect progress and timings ([518cd80](https://github.com/thinkzhou/LunarNX/commit/518cd80d173a18d0bb640471f6989115cb778e8d))
* **ui:** block limited applet-mode launches ([dc4dd22](https://github.com/thinkzhou/LunarNX/commit/dc4dd22ee67dd7e816a551673a8b0e4ebfaf5ae4))
* **ui:** refresh About and support pages ([e28fa07](https://github.com/thinkzhou/LunarNX/commit/e28fa07d8fe5d5e553af05befc081b52f698ab9b))
* **ui:** refresh About and support pages ([84d720b](https://github.com/thinkzhou/LunarNX/commit/84d720b3cd1377944029a9957de25398c38dc889))
* **updater:** show download speed and eta ([b044ba2](https://github.com/thinkzhou/LunarNX/commit/b044ba29245569da8839dbee507c986ee160ab92))
* **updater:** show gzip download progress dialog ([c40266a](https://github.com/thinkzhou/LunarNX/commit/c40266ad3ae858ebd16affbbbb220572f0439701))
* **xbox:** adapt stream policy to network path ([d54e0bd](https://github.com/thinkzhou/LunarNX/commit/d54e0bdd3b5f57fe12d555a0de3b0ee381e64eaf))
* **xcloud:** redesign cloud library browsing ([d8cb64a](https://github.com/thinkzhou/LunarNX/commit/d8cb64a4e1b13be6b70e11e97de097377010af9e))


### Bug Fixes

* **audio:** preserve Audren voice across source flush ([ff72a10](https://github.com/thinkzhou/LunarNX/commit/ff72a105094c8cdc621299b4a1fa1a7534c2ebef))
* **chiaki:** preserve diagnostic patch recount ([2168cd3](https://github.com/thinkzhou/LunarNX/commit/2168cd3572a63c7623067d45f92a1d53ec8ba2d2))
* **ci:** make legacy libpeer patch apply after base patches ([59d04a6](https://github.com/thinkzhou/LunarNX/commit/59d04a622e17631610e33360e597274fe77d0c5e))
* **ci:** preserve nested libpeer dependency patches ([29d6d7c](https://github.com/thinkzhou/LunarNX/commit/29d6d7c722b340a450ab02388a61dbac242445b3))
* **ci:** trust container checkout workspace ([7af9fbf](https://github.com/thinkzhou/LunarNX/commit/7af9fbf8f5a53731bf0707cd0a1becee82b33fc6))
* harden PS pairing and stream reliability ([8e208c5](https://github.com/thinkzhou/LunarNX/commit/8e208c5a1558605cd3946f1664f489fac59e2239))
* improve Xbox stream resilience and latency ([03d00e0](https://github.com/thinkzhou/LunarNX/commit/03d00e0c1ac601e352f74cdbc068f4bf4b8e1657))
* **libpeer:** use reproducible libsrtp headers ([79ffd81](https://github.com/thinkzhou/LunarNX/commit/79ffd81764d7b33af785da42fe354114628b3f64))
* **merge:** remove stale decoder policy reference ([48e8dc5](https://github.com/thinkzhou/LunarNX/commit/48e8dc56aa6c67531807e46dfde4661f0310401a))
* **merge:** restore shared decoder timestamp bookkeeping ([3a08b3a](https://github.com/thinkzhou/LunarNX/commit/3a08b3abf86ee9a47ceab22f94aedc1cf3924743))
* **ps:** align local registration with Chiaki ([ca5f538](https://github.com/thinkzhou/LunarNX/commit/ca5f538f1d3026e15a5bab6c799f789269385bfc))
* **ps:** align persisted LAN pairing with Akira behavior ([4bf994b](https://github.com/thinkzhou/LunarNX/commit/4bf994b8beb404218ae9f7b41080052488df221e))
* **ps:** align PS4 loss recovery with Chiaki ([44f44e0](https://github.com/thinkzhou/LunarNX/commit/44f44e09527122c43f68b358ab350b83259a3d29))
* **ps:** align PS4 pairing firmware targets ([667da86](https://github.com/thinkzhou/LunarNX/commit/667da862829e064ca8bfc7c379cd31aa0bcecce9))
* **ps:** allow persisted LAN routes and periodic host probes ([58f878e](https://github.com/thinkzhou/LunarNX/commit/58f878ee38abbbcdbb3e4c6d6f5336fe8d4d4a70))
* **ps:** buffer video samples before media startup ([e6f378d](https://github.com/thinkzhou/LunarNX/commit/e6f378d73b994bfc6617734bc717701de20e871b))
* **ps:** close review gaps in pairing lifecycle and UX ([9e44d6f](https://github.com/thinkzhou/LunarNX/commit/9e44d6fbdd74c8cad843af0468abc56664a62bd8))
* **ps:** compile transport diagnostics out of release ([1823915](https://github.com/thinkzhou/LunarNX/commit/1823915c2229700e3c49ff545307df264fd470f6))
* **ps:** correct local pairing target and account check ([9e41dd3](https://github.com/thinkzhou/LunarNX/commit/9e41dd38c8f14412b4f56cbbce5b12116c3ba4a6))
* **ps:** defer recovery reset until keyframe ([5be9951](https://github.com/thinkzhou/LunarNX/commit/5be995178aaf2450808c42e18e9d81db5faa05d4))
* **ps:** forward remote play haptics to Switch rumble ([81fbcdf](https://github.com/thinkzhou/LunarNX/commit/81fbcdfa715395d8bdc57ee76e0044a88e6be76f))
* **ps:** harden first-frame media startup ([cb68128](https://github.com/thinkzhou/LunarNX/commit/cb681282036bf5e48488058a8d9144a88cc86c28))
* **ps:** harden local pairing and startup lifecycle ([35f2c4e](https://github.com/thinkzhou/LunarNX/commit/35f2c4e8272e7e348df742e6b7102fde5e6a8c5a))
* **ps:** harden local pairing recovery ([13b83fb](https://github.com/thinkzhou/LunarNX/commit/13b83fbd8e03645edfcdd151c3eef473a3a8feea))
* **ps:** harden post-pair streaming flow ([f7cd293](https://github.com/thinkzhou/LunarNX/commit/f7cd293e3708de5411dc5db17b8a1f62d34431a2))
* **ps:** increase applet socket capacity for pairing ([f63d4b5](https://github.com/thinkzhou/LunarNX/commit/f63d4b5ac49294d37208320a5307e8ff89f54fa0))
* **ps:** make present diagnostics reflect new frames ([27dbf85](https://github.com/thinkzhou/LunarNX/commit/27dbf85248877f77337eecc1475ab455b451a050))
* **ps:** preserve raw pairing failure logs ([3b333c1](https://github.com/thinkzhou/LunarNX/commit/3b333c1f040a16afbb340786101188b60506157f))
* **ps:** prevent background page from stealing stream focus ([1469ed5](https://github.com/thinkzhou/LunarNX/commit/1469ed5587f979a969e43cb1e27d4c5cd5e57a9c))
* **ps:** require accepted NVDEC packets ([73f0290](https://github.com/thinkzhou/LunarNX/commit/73f0290722a0ee499fb19e95404b9e9b37ada7e8))
* **ps:** require Base64 account ID for pairing ([0895702](https://github.com/thinkzhou/LunarNX/commit/0895702c6e06bd206f3f97648e00db98f3cb7f74))
* **ps:** reset decoder after recovery IDR failure ([5db056f](https://github.com/thinkzhou/LunarNX/commit/5db056fe3bed8a33fd0aab8db83cad43f5f03111))
* **ps:** restore vibration for PS Remote Play ([42734d9](https://github.com/thinkzhou/LunarNX/commit/42734d9b9ba9bf43ea3aad38d940166d72585c18))
* **ps:** select console type for manual pairing ([1c96cd8](https://github.com/thinkzhou/LunarNX/commit/1c96cd8ce6bdb7860f448f76810ba4fa0bb9c30e))
* **ps:** separate local pairing identity from PSN ([0145454](https://github.com/thinkzhou/LunarNX/commit/0145454edd6ed64e9e098fd7b028fb4d73cc44df))
* **ps:** simplify pairing account wording ([deecab9](https://github.com/thinkzhou/LunarNX/commit/deecab9f06b927c9b1e98cfdc2a38fe787c3d513))
* **ps:** stabilize pairing focus navigation ([c8d4fc9](https://github.com/thinkzhou/LunarNX/commit/c8d4fc90a00429b7d8e4eaf627452a066f2c604e))
* **ps:** support offline-activated console pairing ([fe9fa15](https://github.com/thinkzhou/LunarNX/commit/fe9fa15dedec448718e33528a4f4612910967b3c))
* **release:** derive dev version from app version ([548be62](https://github.com/thinkzhou/LunarNX/commit/548be624672e826b7d19a08dd8a80f63a05cb05b))
* **release:** reject builds without upload token proof ([ff98159](https://github.com/thinkzhou/LunarNX/commit/ff98159661e0f019cd6ae59fa77bb0aba8f740a4))
* **release:** verify upload token in linked artifact ([f221933](https://github.com/thinkzhou/LunarNX/commit/f221933188ff00fe29e6a02344aaff9ca747ec14))
* **render:** keep NV12 padding out of texture descriptors ([ff9f077](https://github.com/thinkzhou/LunarNX/commit/ff9f077a40ff9a0f0dc84af917db9a784a810569))
* **settings:** separate Xbox and PlayStation controls ([b66073a](https://github.com/thinkzhou/LunarNX/commit/b66073a754b3648fd77825186ecdad0df3158c79))
* **stream:** create stop spinner only on exit ([d543ade](https://github.com/thinkzhou/LunarNX/commit/d543ade0ba81e5b686f8d50719f65d159e5c9a07))
* **stream:** fix 720p green line and settings crash ([f07f1d7](https://github.com/thinkzhou/LunarNX/commit/f07f1d78b9fec359cef1686ebdd1c75b21130ca4))
* **stream:** give quick menu exclusive input ownership ([a75e81e](https://github.com/thinkzhou/LunarNX/commit/a75e81e9fffd8c8e6f943219c0b6324cf34c29b9))
* **stream:** improve loss recovery diagnostics ([f88e5f5](https://github.com/thinkzhou/LunarNX/commit/f88e5f5d0391753471fd8de3353a4ac4fa1102b8))
* **streaming:** align PS/Xbox recovery with tested build ([65ce465](https://github.com/thinkzhou/LunarNX/commit/65ce4659a2383022fddd71e348dc4e80e16b1fbf))
* **stream:** suspend presentation watchdog for child UI ([eb50322](https://github.com/thinkzhou/LunarNX/commit/eb50322a929d7d1cca79e01f9bf8fe01f98bd730))
* **stream:** wait for teardown before leaving stream view ([59fefcc](https://github.com/thinkzhou/LunarNX/commit/59fefcc3444b3871a3b09bf8c98c9630d15de042))
* **tests:** select available C++ compiler in CI ([362ccb2](https://github.com/thinkzhou/LunarNX/commit/362ccb2102e959753714827cefea7e4787e436e8))
* **xbox:** adapt RTP recovery to network quality ([c6da4c1](https://github.com/thinkzhou/LunarNX/commit/c6da4c16c9bf15469aef9f53b9afcfd8f169f4b2))
* **xbox:** bound input and video latency recovery ([19567af](https://github.com/thinkzhou/LunarNX/commit/19567af30f58ac26b39635bd2e7d3265ef64f160))
* **xbox:** close input and watchdog recovery gaps ([ad7de3e](https://github.com/thinkzhou/LunarNX/commit/ad7de3eb2217846ca365b5edeb8e10265446c3a3))
* **xbox:** drop SRTP failure threshold from recovery trigger ([e948ef8](https://github.com/thinkzhou/LunarNX/commit/e948ef8b19769fc251f3743ba456bef6e7bec0b5))
* **xbox:** give home RTP gaps a bounded recovery window ([9cc9e50](https://github.com/thinkzhou/LunarNX/commit/9cc9e503187fe4816dd450e1e1defccafb16a724))
* **xbox:** make gamepad input realtime ([1724eee](https://github.com/thinkzhou/LunarNX/commit/1724eeebd1179ed37646bba0a40f09f0b03f213c))
* **xbox:** make input transitions and SCTP recovery reliable ([c0b43b8](https://github.com/thinkzhou/LunarNX/commit/c0b43b8d321c85e2ca867de160cf24a2d819c703))
* **xbox:** preserve forced input snapshot generation ([ffc3af2](https://github.com/thinkzhou/LunarNX/commit/ffc3af20ca554ed55087e0ddbc30885044f4b101))
* **xbox:** preserve input delivery and sequence continuity ([c332ffa](https://github.com/thinkzhou/LunarNX/commit/c332ffacedd8144dd06d42887f9d1dc0f6520299))
* **xbox:** preserve input transitions under backpressure ([235b716](https://github.com/thinkzhou/LunarNX/commit/235b7169886613904ba275ce628922c4fb5acc1a))
* **xbox:** preserve media on token refresh failure ([005dc18](https://github.com/thinkzhou/LunarNX/commit/005dc18392ee0677332e245fdf43835428facef7))
* **xbox:** preserve recovery budget on high RTT paths ([8b5d363](https://github.com/thinkzhou/LunarNX/commit/8b5d3630b555306a892f98de48811b36299f402d))
* **xbox:** protect cloud RTP recovery from burst loss ([4de7b97](https://github.com/thinkzhou/LunarNX/commit/4de7b978e1e9e7fc9f69367ebd1430f9777f11ef))
* **xbox:** recover long-running video stream stalls ([62ee5b2](https://github.com/thinkzhou/LunarNX/commit/62ee5b237832016a1705c01ca4845f1578ee516a))
* **xbox:** recover stream stalls and preserve input delivery ([76054fa](https://github.com/thinkzhou/LunarNX/commit/76054faa3acd46194a2fbc8122a328e1dc02e177))
* **xbox:** refresh streaming token after keepalive auth failure ([e9a6152](https://github.com/thinkzhou/LunarNX/commit/e9a6152768820cc5a6281b677d9e7d5073bfe2d1))
* **xbox:** restore cloud retransmission jitter budget ([7746b69](https://github.com/thinkzhou/LunarNX/commit/7746b6966900e167695f687fe7c38044630930e6))
* **xbox:** restore known-good UDP receive buffer ([df38e1f](https://github.com/thinkzhou/LunarNX/commit/df38e1f3ddb97de72e8d0d08d9ab0bbd5cdc9e3a))
* **xbox:** restore stable input cadence ([275a170](https://github.com/thinkzhou/LunarNX/commit/275a1701c4a8dec8d06a2ce00b078a887da5ca09))
* **xbox:** retry latest input and reset audio source ([2782065](https://github.com/thinkzhou/LunarNX/commit/278206590696b521b5e8ae534138483229082096))
* **xbox:** stabilize low-latency audio playback ([b58232b](https://github.com/thinkzhou/LunarNX/commit/b58232bee8b3106269694caec960422d0867aa33))
* **xbox:** stabilize realtime streaming and recovery ([04b48b1](https://github.com/thinkzhou/LunarNX/commit/04b48b1f509096868c230e07bfddda9a41beaef1))
* **xbox:** stabilize realtime streaming and recovery ([c8e4fb9](https://github.com/thinkzhou/LunarNX/commit/c8e4fb977b8eac9877ddebad3fe60cb7376bd3d2))


### Performance Improvements

* **ps:** buffer decoded frames across present jitter ([6ae4b59](https://github.com/thinkzhou/LunarNX/commit/6ae4b59a7c15178161ab2bf8b7eb8bad039bdb73))
* **ps:** stabilize 30 Mbps high-bitrate streaming ([43bd12c](https://github.com/thinkzhou/LunarNX/commit/43bd12ca3fa1717177ba892b076ef9405f7c3802))
* **stream:** separate Xbox and PlayStation video paths ([6f55c77](https://github.com/thinkzhou/LunarNX/commit/6f55c77d2ebf7f551caa33a9cd382d705eeccf12))


### Documentation

* refresh readme and add app store banner ([714f3aa](https://github.com/thinkzhou/LunarNX/commit/714f3aaaf927ceb37078bf91a713f3abad92a8bb))
* refresh README and add app store banner ([9e5d71c](https://github.com/thinkzhou/LunarNX/commit/9e5d71c04a77247745035b8c9e889b3fddba582d))
* **stream:** PS/Xbox video decoder separation design ([c9db685](https://github.com/thinkzhou/LunarNX/commit/c9db68563853bffb000f779fb851e67a1e4128b2))


### Code Refactoring

* **stream:** own PS/Xbox decode policy in separate decoder classes ([a26a02c](https://github.com/thinkzhou/LunarNX/commit/a26a02ce8109075a38b14f16bc6b88068bdbbf05))

## 0.2.0 (2026-08-14)

This version is the baseline for automated releases.
