#!/usr/bin/env python3
r"""HTArcadePak - builds HTArcade.pak for HTFrameFix from the Hydro Thunder ARCADE hard disk.

The arcade version's sounds and music are on its hard disk (the D: drive) as *.CSH
files: sound banks the game decoded to 16-bit 31250 Hz audio. This script takes those
for the PC sounds they match and writes HTArcade.pak; put it next to HTFrameFix.asi.
Nothing of the arcade's is in HTFrameFix itself: you need your own copy of the disk.

  1. a raw image of the disk. From a MAME CHD (hydro_101b.chd and so on):
        chdman extracthd -i hydro_101b.chd -o hydro.img
     (chdman comes with MAME). A dump made with dd works as well, or a folder with the
     *.CSH files from the disk's D: drive and HT.R2 from its C: drive (in it or in subfolders).
  2. python HTArcadePak.py hydro.img            (or: python HTArcadePak.py <folder>)
     writes HTArcade.pak in the current folder (-o to put it elsewhere).
     --fsd path\to\Hydro.fsd checks it is the PC data the map was made for.

Python 3.6+, nothing else needed.
"""
import argparse, array, os, struct, sys

# PC sound (its ESF's offset in Hydro.fsd) : arcade bank : stream : gain (/4096) : flags (1 music, 2 loop)
MAP = (
    "16744c:GAME:0:3995:0 167746:GAME:1:4096:0 167a86:GAME:10:4111:0 168bfe:GAME:78:3209:0 "
    "16aa43:VENI:3:4096:2 172742:CHIN:2:3963:0 174b01:CHIN:3:3971:0 177752:CHIN:4:3979:0 "
    "179afa:NILE:6:3641:0 1817e9:CHIN:5:3966:0 183d07:WRAP:2:3646:0 185c31:LOO1:7:4083:2 "
    "18bd9b:LOO1:8:4109:2 19303e:GAME:11:4054:0 194126:COMM:2:3648:0 19dec7:CUTT:0:3665:0 "
    "19f672:CUTT:1:3652:2 1a0600:CUTT:2:2591:0 1a278c:GAME:80:4094:0 1a3f0a:GAME:81:4072:0 "
    "1a531c:GAME:82:4046:0 1a6a9a:GAME:12:4066:0 1a77c2:GAME:86:4030:0 1b11e4:SEC1:0:4092:0 "
    "1b6d79:GAME:89:3633:2 1b9f0d:GAME:90:3624:2 1bae9b:GAME:91:3640:2 1bd027:GAME:92:3656:2 "
    "1beeb4:GAME:93:3640:2 1c02ef:GAME:13:4029:0 1c1197:CHIN:6:4096:2 1cd9d5:COMM:5:3644:2 "
    "1ce691:MISS:0:3647:0 1d123f:THRE:0:3637:0 1d4080:TIDA:0:3262:0 1d765d:GAME:14:4031:0 "
    "1d843e:SEC1:1:3667:0 1deb45:SEC4:0:3644:0 1e4c82:BANS:0:4096:0 1e6f3f:CUTT:3:3641:0 "
    "1e8d55:MIDW:0:3652:0 1ed9f5:RADH:2:4096:0 1f0d75:RAZR:0:3245:0 1f593d:SEC2:0:3651:0 "
    "1f8372:SEC3:0:3646:0 1fc358:DAMN:0:3654:0 1ff552:GAME:15:4048:0 203614:NEWY:13:3940:2 "
    "20566f:WRAP:4:3635:0 2073df:WRAP:5:3639:0 2098af:LOO1:9:4096:0 20b277:GRAV:2:3649:0 "
    "20fbd7:CUTT:2:2906:0 211d63:MIDW:2:2570:0 2125b2:MIDW:2:3624:0 212e01:RADH:4:4102:0 "
    "219209:GAME:16:4072:0 219fbd:MIDW:2:3624:0 21a80c:CUTT:2:3656:0 21c998:CUTT:2:3656:0 "
    "21eb24:CUTT:2:3656:0 220cb0:CUTT:2:3656:0 222e3c:GRAV:3:4096:2 22cada:GAME:103:4076:0 "
    "22e5a7:GAME:104:4077:0 23187d:GAME:17:4096:0 232b05:NEWY:16:4087:0 2361c9:GREE:7:4092:1 "
    "2612e3:WRAP:6:3626:0 263bc1:GAME:105:2562:0 265e73:GAME:108:2143:0 26636c:WRAP:7:4095:0 "
    "269ea0:GAME:110:4084:0 26d339:GAME:111:3623:2 26efeb:GAME:112:4087:2 272593:GAME:18:4065:0 "
    "27399b:CHIN:8:4086:0 2759e0:GAME:113:4100:0 277ab7:GAME:114:3605:0 279a47:GAME:115:3643:0 "
    "27b198:GAME:116:3665:0 27d78a:GAME:117:3632:0 27f0b3:GAME:119:3631:0 280553:GAME:120:3645:0 "
    "280fb2:GAME:121:3630:0 282144:WRAP:9:4096:0 2855cc:GAME:19:4062:0 286874:AMAZ:5:3970:0 "
    "28a051:AMAZ:7:4039:0 28e68e:COMM:6:4096:2 28f567:PWEL:5:3647:2 292a5b:GAME:122:4098:0 "
    "294963:COMM:7:4096:0 29691a:NILE:8:3654:0 29955f:NILE:9:3679:2 299f17:WRAP:10:3630:0 "
    "29bf67:ARCT:8:4087:0 2a2782:GAME:2:4046:0 2a352a:GAME:21:4091:0 2a4c82:GAME:123:4080:0 "
    "2a766f:GAME:124:4077:0 2a83b8:NEWY:19:3656:2 2aff68:GAME:125:4076:0 2b2a7b:COMM:8:3641:0 "
    "2bc1bb:COMM:9:3600:0 2c022c:GAME:22:4031:0 2c691d:MISS:3:3642:2 2c7c0d:MISS:4:3640:0 "
    "2c93cf:AMAZ:10:4069:2 2cf7dc:GRAV:4:4077:0 2d2b58:GAME:127:4077:0 2d4974:GRAV:5:4078:0 "
    "2d6b6c:AMAZ:11:3967:2 2dba7b:NILE:10:4083:0 2dd093:GAME:23:4016:0 2df5bb:ARCT:9:3974:0 "
    "2e3d07:NILE:11:4096:2 2e72e0:NILE:13:4093:2 2ecd84:NILE:14:4083:2 2f11ec:WRAP:14:3631:0 "
    "2f3dca:WRAP:15:3615:0 2f5ee5:NEWY:28:3641:0 2fb802:ARCT:11:3644:0 302433:GAME:128:3244:0 "
    "303ced:NILE:17:3656:0 305ddb:GAME:24:4027:0 306bfb:GRAV:12:4086:0 30bd18:NEWY:29:3645:0 "
    "3118f1:CUTT:0:4096:0 31309c:MIDW:3:3608:0 3195aa:RAZR:3:3591:0 31e7e3:CUTT:0:3665:0 "
    "31ff8e:CUTT:0:3665:0 321739:CUTT:0:3665:0 322ee4:TIDA:2:3641:0 329769:GAME:25:4003:0 "
    "32b231:SEC1:5:3634:0 32ead9:ARCT:12:4098:0 331e63:PWEL:7:4084:0 3400d8:PWEL:9:4072:0 "
    "343e20:PWEL:10:4076:0 347e48:PWEL:11:4077:0 34c394:VENI:7:4066:0 34fd58:VENI:8:4069:0 "
    "353df0:GAME:26:4003:0 355e08:VENI:9:4087:0 35a298:VENI:10:4088:0 35f979:ARCT:14:3969:0 "
    "3617ef:WRAP:16:4096:0 364ebd:ARCT:15:3971:0 36b87f:ARCT:16:3975:0 370387:ARCT:17:3961:0 "
    "377ee7:WRAP:17:3619:0 37a0e4:GAME:129:4063:0 37c3ec:GRAV:13:4088:2 380331:GAME:30:3987:0 "
    "3827f9:ARCT:18:4089:0 3848e6:ARCT:19:4084:2 38ff1c:MISS:5:3636:0 39297a:THRE:2:3646:0 "
    "3966e5:SEC2:4:3655:2 397c12:BANS:4:4096:2 39919a:MIDW:5:3641:2 39ace3:RAZR:4:3629:2 "
    "39c2d3:SEC1:6:3670:2 39d9fb:GAME:31:4014:0 39fa03:BANS:4:3657:2 3a0f8b:BANS:4:3657:2 "
    "3a2513:SEC4:6:3646:0 3a762f:BANS:5:4096:0 3ad098:CUTT:5:3640:0 3b487e:RADH:5:3651:0 "
    "3bc686:RAZR:5:4096:0 3c4090:SEC2:5:3654:0 3c75b5:SEC3:5:3650:0 3cbb8a:DAMN:2:3647:0 "
    "3d23c1:GAME:32:4018:0 3d4689:COMM:10:3609:0 3dadda:COMM:11:3623:0 3e1168:LOO1:11:4096:0 "
    "3e2a50:NEWY:30:3667:2 3e4bba:GAME:33:3995:0 3e9e70:NEWY:31:4095:2 3ecdd8:GAME:132:3992:0 "
    "3ede26:GAME:133:4079:0 3f0121:GAME:134:4066:0 3f0df3:GAME:135:4063:0 3f3fd5:GAME:136:4005:0 "
    "3f66ac:GAME:137:4092:0 3fc6aa:GRAV:15:4096:2 400433:GAME:3:3996:0 401ddb:GAME:34:4018:0 "
    "4197e3:GAME:35:4020:0 41f3b3:GAME:138:3975:0 421a3b:GRAV:16:4092:2 4276e4:WRAP:18:3634:0 "
    "4298cb:GAME:139:4099:0 42bf13:GAME:140:4084:0 42fea3:GRAV:17:4094:0 4372b4:GRAV:18:4086:0 "
    "43e1de:GRAV:19:4088:0 44419b:GRAV:20:4079:0 44946c:GAME:37:4012:0 46621e:WRAP:19:3664:0 "
    "46938a:COMM:12:3626:0 477eae:GAME:142:4077:0 47a0b6:AMAZ:15:4019:2 47d98b:ARCT:22:4092:2 "
    "481bfb:WRAP:20:3611:0 483a83:GAME:143:4087:0 484d3e:GAME:38:4008:0 48ed67:COMM:14:3626:0 "
    "4909ec:CUTT:0:3665:0 492197:CUTT:1:3652:2 493125:CUTT:2:3656:0 4952b1:GAME:144:3200:0 "
    "49dd3e:COMM:15:3623:0 49fc8e:GAME:39:4056:0 4a6a79:CUTT:0:3665:0 4a8224:CUTT:1:3652:2 "
    "4a91b2:CUTT:2:3656:0 4ab33e:GAME:145:4026:0 4ad5c6:GAME:146:3250:0 4af531:GAME:147:3555:0 "
    "4b1809:WRAP:21:3625:0 4b450c:COMM:16:4096:0 4b6455:GAME:40:4047:0 4b7b3d:CUTT:0:3665:0 "
    "4b92e8:CUTT:1:4096:2 4ba276:CUTT:2:2591:0 4bc402:PWEL:16:4040:2 4c27e3:GAME:148:4088:0 "
    "4c3c61:COMM:17:4093:0 4c517d:COMM:18:4078:0 4c6a4e:WRAP:22:3618:0 4c9152:NILE:24:3658:0 "
    "4cd607:GAME:149:4082:2 4cf30f:ARCT:23:3973:2 4d5dfd:GAME:150:3630:0 4d793e:GAME:151:4091:0 "
    "4db746:GAME:152:4091:0 4dddee:ARCT:24:4052:2 4e23ba:GAME:153:3033:0 4f473a:GAME:156:3324:0 "
    "4fc429:GAME:46:4032:0 4fe3f1:COMM:19:4076:2 5040fc:COMM:20:4085:0 506626:WRAP:23:3643:0 "
    "50860e:WRAP:24:3653:0 50a446:WRAP:25:3778:0 50bb8a:WRAP:26:3670:0 50c51e:WRAP:27:3642:0 "
    "50f526:WRAP:28:4054:0 50fb47:GAME:157:4073:0 510f16:NILE:25:3647:0 5183a0:GAME:47:4043:0 "
    "520d4a:GAME:49:4048:0 522952:GAME:4:4073:0 52384a:GAME:51:3989:0 53b281:GAME:53:4066:0 "
    "553c6f:GAME:55:3947:0 5654c9:GAME:56:4021:0 5822d6:GAME:57:4053:0 58b9c9:GAME:58:4057:0 "
    "58d011:GAME:59:4070:0 58e8a9:GAME:60:4037:0 591851:GAME:5:4044:0 599a8f:NEWY:0:3640:0 "
    "5aa7fb:WRAP:0:3657:0 5ac7ac:GAME:62:4096:0 5ae7ed:NEWY:1:4093:0 5b135a:GAME:64:6573:2 "
    "5b3de4:VENI:0:2582:2 5b99d4:NEWY:4:4090:2 5bf5c5:GAME:6:4068:0 5c5127:VENI:1:3643:2 "
    "5cb58b:GAME:65:4084:0 5ccae3:GAME:66:3991:0 5d28c5:AMAZ:0:4089:2 5da5b4:NILE:3:4096:2 "
    "5dfd62:NILE:4:3646:2 5e51cb:GAME:67:4067:0 5e6ed3:GRAV:0:4090:2 5ee01e:GAME:7:4053:0 "
    "5ef5a6:NILE:5:4096:2 5f3737:GAME:69:2883:0 5fa6d2:GAME:70:4096:0 5fbc0a:COMM:0:3517:0 "
    "6011a2:PWEL:1:4096:2 6089d4:GAME:8:4096:0 60928c:LOO1:4:4092:2 60c692:GAME:71:3597:0 "
    "60e54f:VENI:2:4096:0 6123d5:GAME:72:4051:0 6131ee:GAME:73:4077:0 614047:GAME:74:4096:0 "
    "615395:GAME:75:4015:0 6161ee:GAME:76:4082:0 617047:GAME:77:4096:0 617eaf:LOO1:5:4084:2 "
    "61c93c:GAME:9:4048:0 61dcc4:LOO1:6:4090:2 629b5f:GREE:2:3652:0 62e41f:GREE:3:4088:0 "
    "632fe8:GREE:4:4092:0 637d86:COMM:1:3644:0 63961c:CHIN:1:4092:0 63d30e:AMAZ:3:3960:0 "
    "63ecb7:AMAZ:9:2454:1 7310c1:ARCT:5:2293:1 8234cb:ARCT:3:2986:1 8740d1:ARCT:4:2107:1 "
    "9664db:PWEL:13:2648:1 a07cdf:NEWY:18:4074:1 a588e5:PWEL:0:2662:1 aa94eb:PWEL:14:4122:0 "
    "aba96a:NEWY:25:2192:1 b5c16e:NEWY:24:2646:1 bacd74:NILE:16:2556:1 c9f17e:CHIN:0:2624:1 "
    "d91588:GRAV:8:2194:1 e83992:GRAV:8:2194:1 f25196:GRAV:11:2272:1 f75d9c:GRAV:10:2241:1 "
    "fc69a2:VENI:6:2379:1 10b8dac:GREE:7:2606:1 115a5b0:WRAP:11:3444:1 11fbdb4:WRAP:12:3917:1 "
    "12665bc:WRAP:29:2116:0 12c237d:LOO1:10:2570:1 1405385:GAME:141:2339:0 1411d04:GAME:109:1989:0 "
)
# engine and drone loops, raw 16-bit PCM in the disk's C:\HT.R2 (not in the caches):
# PC sound : HT.R2 object : gain (/4096) : flags : rate. Played at the rate the PC's own
# copies were made for, so the game's pitch control sounds as before.
ENGINES = (
    "5255c2:S_SBANSLOA0:3620:2:15625 5269d0:S_SBANSMEA0:3528:2:15625 527f47:S_SBANSHIA0:3582:2:15625 "
    "528df3:S_SCUTTLOA0:3180:2:44100 52b09a:S_SCUTTMEA0:3642:2:15625 52f4a2:S_SCUTTHIA0:3246:2:15625 "
    "53158a:S_SDAMNLOA0:3631:2:15625 5362d5:S_SDAMNMEA0:3650:2:15625 537b5a:S_SDAMNHIA0:3653:2:15625 "
    "53a152:S_SDRON_AA0:3636:2:15625 53d439:S_SDRON_BA0:3685:2:15625 53feae:S_SDRON_CA0:3623:2:15625 "
    "5418b6:S_SDRON_DA0:3497:2:15625 5445fe:S_SDRON_HA0:1489:2:15625 545aa7:S_SMIDWLOA0:3654:2:15625 "
    "54b910:S_SMIDWMEA0:3663:2:15625 54ce0e:S_SMIDWHIA0:3644:2:15625 54d535:S_SMISSLOA0:3650:2:15625 "
    "550db0:S_SMISSMEA0:3651:2:15625 5520ce:S_SMISSHIA0:3644:2:15625 555477:S_SRADHLOA0:3560:2:15625 "
    "5579f4:S_SRADHMEA0:3587:2:15625 558cb7:S_SRADHHIA0:3635:2:15625 559878:S_SRAZRLOA0:3553:2:15625 "
    "55ae1e:S_SRAZRMEA0:3651:2:15625 55c357:S_SRAZRHIA0:3635:2:15625 55da63:S_SSEC1LOA0:3645:2:15625 "
    "55f009:S_SSEC1MEA0:3645:2:15625 560542:S_SSEC1HIA0:3645:2:15625 561c4e:S_SSEC2LOA0:3645:2:15625 "
    "566d51:S_SSEC2MEA0:3645:2:15625 56806f:S_SSEC2HIA0:3645:2:15625 569c10:S_SSEC3LOA0:3645:2:15625 "
    "56e95b:S_SSEC3MEA0:3645:2:15625 5701e0:S_SSEC3HIA0:3645:2:15625 5727d8:S_SSEC4LOA0:3645:2:15625 "
    "574a7f:S_SSEC4MEA0:3645:2:15625 578e87:S_SSEC4HIA0:3645:2:15625 57af6f:S_STHRELOA0:3652:2:15625 "
    "580dd8:S_STHREMEA0:3663:2:15625 58408e:S_STHREHIA0:3646:2:15625 585528:S_STIDALOA0:3518:2:15625 "
    "588866:S_STIDAMEA0:3617:2:15625 589b6b:S_STIDAHIA0:3630:2:15625 "
)
BANKS = ("AMAZ ARCT BANS CHIN COMM CUTT DAMN DIAG GAME GRAV GREE LOO1 LOO2 MIDW MISS NEWY NILE PWEL "
         "RADH RAZR SEC1 SEC2 SEC3 SEC4 THRE TIDA VENI WRAP").split()
RATE = 31250


class Fat:
    """Read-only FAT12/16/32 volume: just enough to read files in its root folder."""
    def __init__(self, f, base):
        self.f, self.base = f, base
        b = self.read(0, 512)
        self.bps, self.spc = struct.unpack_from("<HB", b, 11)
        res, nfat, rootn, tot16, _, fatsz16, = struct.unpack_from("<HBHHBH", b, 14)
        if self.bps not in (512, 1024, 2048, 4096) or not self.spc or not nfat:
            raise ValueError("not FAT")
        fatsz = fatsz16 or struct.unpack_from("<I", b, 36)[0]
        tot = tot16 or struct.unpack_from("<I", b, 32)[0]
        self.fat = res * self.bps
        rootsec = (rootn * 32 + self.bps - 1) // self.bps
        self.root = (res + nfat * fatsz) * self.bps
        self.data = self.root + rootsec * self.bps
        clusters = (tot - res - nfat * fatsz - rootsec) // self.spc
        self.kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        self.rootn = rootn
        self.root32 = struct.unpack_from("<I", b, 44)[0] if self.kind == 32 else 0
        self.csize = self.bps * self.spc

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def next(self, c):
        if self.kind == 16:
            return struct.unpack("<H", self.read(self.fat + c * 2, 2))[0]
        if self.kind == 32:
            return struct.unpack("<I", self.read(self.fat + c * 4, 4))[0] & 0x0FFFFFFF
        v = struct.unpack("<H", self.read(self.fat + c * 3 // 2, 2))[0]
        return v >> 4 if c & 1 else v & 0xFFF

    def chain(self, c):
        end = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.kind]
        seen = 0
        while 2 <= c < end and seen < 4000000:
            yield c
            c = self.next(c)
            seen += 1

    def readfile(self, c, size):
        out = bytearray()
        for cl in self.chain(c):
            out += self.read(self.data + (cl - 2) * self.csize, self.csize)
            if len(out) >= size:
                break
        return bytes(out[:size])

    def files(self):
        if self.kind == 32:
            raw = self.readfile(self.root32, 1 << 20)
        else:
            raw = self.read(self.root, self.rootn * 32)
        for i in range(0, len(raw) - 31, 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x18 or e[11] == 0x0F:
                continue
            name = e[0:8].decode("latin-1").rstrip() + "." + e[8:11].decode("latin-1").rstrip()
            c = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16 if self.kind == 32 else 0)
            yield name.upper(), c, struct.unpack_from("<I", e, 28)[0]


def partitions(f):
    """(byte offset) of every FAT partition in an MBR disk image, extended ones included."""
    def table(sector, ext_base, depth=0):
        if depth > 64:                    # a looping partition chain
            return
        f.seek(sector * 512)
        b = f.read(512)
        if len(b) < 512 or b[510:512] != b"\x55\xaa":
            return
        for i in range(4):
            t = b[446 + i * 16 + 4]
            lba = struct.unpack_from("<I", b, 446 + i * 16 + 8)[0]
            if not t or not lba:
                continue
            if t in (0x05, 0x0F, 0x85):
                base = ext_base or lba
                yield from table((ext_base or 0) + lba if ext_base else lba, base, depth + 1)
            else:
                yield ((sector if ext_base else 0) + lba) * 512
    return list(table(0, 0))


def find_csh(src):
    """{bank name: CSH bytes, "HT.R2": bytes} from a folder or a disk image."""
    found = {}
    if os.path.isdir(src):                # (subfolders too: the C: and D: drives copied side by side)
        for root, _, names in os.walk(src):
            for n in sorted(names):
                u = n.upper()
                key = "HT.R2" if u == "HT.R2" else u[1:5] if u.endswith(".CSH") and u[0] == "S" else None
                if (key == "HT.R2" or key in BANKS) and key not in found:
                    with open(os.path.join(root, n), "rb") as fh:
                        found[key] = fh.read()
        return found
    with open(src, "rb") as f:
        bases = partitions(f) or [0]
        for base in bases:
            try:
                fat = Fat(f, base)
            except (ValueError, struct.error):
                continue
            for name, c, size in fat.files():
                if name.endswith(".CSH") and name[0] == "S" and name[1:5] in BANKS and name[1:5] not in found:
                    found[name[1:5]] = fat.readfile(c, size)
                elif name == "HT.R2" and "HT.R2" not in found:
                    found["HT.R2"] = fat.readfile(c, size)
    return found


def r2_objects(r2, names):
    """{name: bytes} of the named objects in an HT.R2 archive."""
    dirofs, n = struct.unpack_from("<I", r2, 0x20)[0], struct.unpack_from("<I", r2, 0x2C)[0]
    out = {}
    for i in range(n):
        p = dirofs + i * 0x4C
        off, size = struct.unpack_from("<II", r2, p)
        name = r2[p + 12:p + 0x4C].split(b"\0")[0].decode("latin-1")
        if name in names:
            out[name] = r2[off:off + size]
    return out


def streams(csh):
    if csh[:4] != b"CASH":
        raise ValueError("not a sound cache")
    hb, n = struct.unpack_from("<II", csh, 8)
    sizes = struct.unpack_from("<%dI" % n, csh, 16)
    o = 16 + 8 * n + hb
    out = []
    for s in sizes:
        out.append(csh[o:o + s])
        o += s
    if o != len(csh):
        raise ValueError("unexpected cache layout")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", help="arcade disk image (e.g. from chdman extracthd), or a folder with its *.CSH files and HT.R2")
    ap.add_argument("-o", "--out", default="HTArcade.pak")
    ap.add_argument("--fsd", help="the PC version's Hydro.fsd, to check it matches")
    a = ap.parse_args()

    ents = []
    for tok in MAP.split():
        off, bank, idx, gain, fl = tok.split(":")
        ents.append((int(off, 16), bank, int(idx), int(gain), int(fl)))

    if a.fsd:
        with open(a.fsd, "rb") as f:
            for off in [e[0] for e in ents] + [int(t.split(":")[0], 16) for t in ENGINES.split()]:
                f.seek(off)
                if f.read(4) != b"ESF\x08":
                    sys.exit("%s is not the Hydro.fsd this map was made for (no sound at 0x%X)" % (a.fsd, off))

    csh = find_csh(a.source)
    need = sorted({e[1] for e in ents})
    missing = [b for b in need if b not in csh]
    if missing:
        sys.exit("sound caches not found: %s\n(expected S<bank>AA0.CSH files on the arcade disk's D: drive)"
                 % ", ".join("S%sAA0.CSH" % b for b in missing))
    banks = {b: streams(csh[b]) for b in need}
    engs = []
    for tok in ENGINES.split():
        off, name, gain, fl, rate = tok.split(":")
        engs.append((int(off, 16), name, int(gain), int(fl), int(rate)))
    if "HT.R2" not in csh:
        sys.exit("HT.R2 not found (the arcade disk's C: drive) - it has the engine sounds")
    objs = r2_objects(csh["HT.R2"], {e[1] for e in engs})
    missing = [e[1] for e in engs if e[1] not in objs]
    if missing:
        sys.exit("HT.R2 lacks %s" % ", ".join(missing))
    engoff = {e[0] for e in engs}
    ents = [(off, bank, idx, gain, fl, RATE) for off, bank, idx, gain, fl in ents if off not in engoff]
    ents += [(off, "R2", name, gain, fl, rate) for off, name, gain, fl, rate in engs]
    ents.sort()

    def pcm(bank, idx):
        if bank == "R2":
            return objs[idx][8:]          # 8-byte header, then 16-bit PCM
        return banks[bank][idx]

    blobs, order, table = {}, [], []
    for off, bank, idx, gain, fl, rate in ents:
        key = (bank, idx, gain)
        if key not in blobs:
            src = array.array("h")
            raw = pcm(bank, idx)
            src.frombytes(raw[:len(raw) & ~1])
            if sys.byteorder == "big":
                src.byteswap()
            if gain == 4096:
                dst = src
            else:
                dst = array.array("h", (max(-32768, min(32767, (s * gain + 2048) >> 12)) for s in src))
            if sys.byteorder == "big":
                dst.byteswap()
            blobs[key] = dst.tobytes()
            order.append(key)
        table.append((off, key, fl, rate))

    pos = 12 + 20 * len(table)
    where = {}
    for k in order:
        where[k] = pos
        pos += len(blobs[k])
    with open(a.out, "wb") as f:
        f.write(b"HTAP" + struct.pack("<II", 1, len(table)))
        for off, k, fl, rate in table:
            f.write(struct.pack("<IIIII", off, rate, len(blobs[k]) // 2, fl, where[k]))
        for k in order:
            f.write(blobs[k])
    music = sum(1 for _, _, fl, _ in table if fl & 1)
    print("%s: %d sounds (%d music tracks), %.1f MB" % (a.out, len(table), music, pos / 1e6))


if __name__ == "__main__":
    main()
