# Nestbox di VPS: mana yang bisa menjalankan replica

Nestbox versi Ubuntu (`sudo tools/linux/host/nestbox`, lihat
[tools/linux/host/](../tools/linux/host/)) menjalankan replica langsung di
mesin tempat ia dipasang, sebagai guest KVM. Syarat mutlaknya satu:
**mesin itu harus punya `/dev/kvm`**. Di server fisik itu tinggal
mengaktifkan VT-x / AMD-V di firmware. Di VPS, keputusannya ada di tangan
provider: hypervisor mereka harus mengekspos ekstensi virtualisasi CPU ke
VPS Anda ("nested virtualization"). Kalau tidak, tidak ada yang bisa
diatur dari dalam VPS, dan membuat VM lebih dulu juga tidak menolong,
karena VM itu pun butuh KVM.

Tes satu baris setelah masuk SSH, sebelum membeli lebih lama atau
memasang apa pun:

```bash
ls -l /dev/kvm && grep -c -E 'vmx|svm' /proc/cpuinfo
```

Kalau `/dev/kvm` ada dan hitungan flag-nya lebih dari nol, Nestbox bisa
membuat replica di sana. Kalau salah satunya tidak ada, baris PC di UI
Nestbox menampilkan "no /dev/kvm" dan tombol "+" tidak akan berhasil.

Data di bawah dikumpulkan 5 September 2026: hasil cek langsung ke server
yang ada, dan dokumentasi resmi provider (tautan di bagian akhir). Harga
dan kebijakan bisa berubah; cek ulang sebelum membeli.

## Hasil cek langsung

| Server | Provider | Spek | `/dev/kvm` | Replica? |
|---|---|---|---|---|
| OVHcloud VPS | OVHcloud | 6 vCPU Haswell, 11,6 GB RAM, 96 GB disk | **Ya**, 12 flag vmx | Ya, langsung |
| Niagahoster VPS | Niagahoster | 2 vCPU EPYC, 8 GB RAM, 96 GB disk | Tidak | Tidak |
| Hetzner Cloud CPX | Hetzner | 8 vCPU, 15,6 GB RAM, 75 GB disk | Tidak | Tidak (FAQ resmi Hetzner Cloud juga menolak nested) |
| Contabo VPS | Contabo | tidak dicek | tidak dicek | VPS biasa: tidak. Hanya VDS dan dedicated Contabo |

## VPS (bukan bare metal) yang mengekspos KVM, dengan lokasi Singapura

| Provider | Nested KVM | Harga mulai | Lokasi Asia | Refund |
|---|---|---|---|---|
| OVHcloud VPS | Ya, terbukti langsung di server yang dicek (`/dev/kvm` ada, flag vmx terekspos) | sekitar US$4.54/bulan (Starter) sampai US$23.37/bulan; di APAC ada kuota bandwidth bulanan (500 GB VPS-1, 1 TB VPS-2/3, 3 TB VPS-4), lewat itu dibatasi 10 Mbps | Singapura (region SGP, sejak 2016), Sydney | Hak pembatalan 14 hari untuk individu dan pesanan baru, dikembalikan pro-rata dikurangi hari terpakai, dalam 30 hari |
| ExtraVM | Ya, aktif default di semua VPS. **Stok Singapura sering habis** (per 5 September 2026 kosong); Tokyo dan Sydney kadang masih ada | sekitar $4.50/bulan | Singapura (Equinix SG3), Tokyo, Sydney | 5 hari money-back untuk VPS, potongan 4% untuk refund di atas $25, pembayaran kripto tidak bisa refund |
| **Advin Servers** | Ya, ditulis sendiri di halaman depan mereka: "KVM & Nested Virtualization" | EPYC Genoa 9654: 2 vCPU / 4 GB / 64 GB NVMe / 2 TB $6/bulan; 8 vCPU / 16 GB / 256 GB / 10 TB $20/bulan; di Singapura EPYC 9375F (3,8 GHz) 2 vCPU / 8 GB / 40 GB $19,90 | Singapura, **Johor** (Equinix JH1, backhaul ke Equinix SG1), Tokyo, Osaka | 14 hari money-back tanpa syarat dan tanpa alasan; setelah itu kredit akun pro-rata |
| **Onidel Cloud** | Ya, aktif default di KVM mereka; benchmark pihak ketiga di VPS Singapura EPYC 7713P mencatat nested "Yes", dan mereka tetap membiarkannya menyala setelah isu keamanan (host dipatch, fiturnya tidak dimatikan) | Premium EPYC Milan, harga Singapura per bulan (juga bisa per jam): ONI-1 1 core / 2 GB / 20 GB / 1 TB $4,95; ONI-2 2 / 4 GB / 40 GB / 2 TB $9,90; ONI-3 4 / 8 GB / 80 GB / 4 TB $19,80; ONI-4 6 / 12 GB / 120 GB / 6 TB $29,70; ONI-5 8 / 32 GB / 240 GB / 8 TB $65,60; ONI-6 12 / 48 GB / 320 GB / 12 TB $95. Diskon 5-30% untuk kontrak 3 bulan sampai 3 tahun. **High Frequency EPYC Turin (Zen 5, 3,8+ GHz, Geekbench 6 satu core 2976 menurut Onidel)**, Singapura saja: HF-1 1 core / 2 GB / 20 GB / 1 TB $6,45; HF-2 2 / 4 GB / 40 GB / 2 TB $12,90; HF-3 4 / 8 GB / 80 GB / 4 TB $25,80; HF-4 6 / 12 GB / 120 GB / 6 TB $38,70; HF-5 8 / 32 GB / 240 GB / 8 TB $86,40; HF-6 12 / 48 GB / 320 GB / 12 TB $126,20. Tagihan per jam tersedia (HF-2 $0,0192/jam, HF-4 $0,0576/jam) | Singapura (Turin hanya di sini), Sydney, Melbourne, Ho Chi Minh, Amsterdam, New York | 7 hari money-back |
| Cloudzy | Diklaim di halaman pemasaran dan blog mereka ("nested KVM"), tidak ada di halaman produk — konfirmasi dulu | EPYC 9554 Genoa: 512 MB $2,48; 2 GB / 60 GB $7,48; 4 GB / 120 GB / 2 vCPU $14,48 (harga diskon 50%) | Singapura (ap-sgp-1), 13 region | 14 hari money-back, batal sendiri lewat panel |
| GreenCloud VPS | Ya. Review pihak ketiga dari Indonesia di plan Budget KVM Singapura DC2 menunjukkan "VM-x/AMD-V: Enabled" dan memakai nested. Selain lini EPYC, ada lini **Ryzen 9950X** (RyzenKVM, 3,5+ GHz) yang juga ada di Singapura DC2 | Budget/SSD KVM dari $6/bulan (1 core, 1 GB RAM, 15 GB SSD — terlalu kecil untuk replica, ambil minimal SSDKVM-3 $20/bulan dengan 30 GB); plan promo tahunan pernah $25/tahun untuk 2 core EPYC, 4 GB RAM, 35 GB NVMe | Singapura DC1 & DC2, Tokyo, Hong Kong, Hanoi, Ho Chi Minh | 7 hari untuk VPS pertama di akun baru; plan diskon/promo dan pembayaran kripto tidak termasuk |
| SSD Nodes | Ya, aktif default di semua plan | sekitar $14.50/bulan untuk 32 GB RAM, tapi kontrak 3 tahun dibayar di muka | Singapura, Tokyo, Mumbai, Sydney | 14 hari full refund lewat tiket support; setelah itu hanya kredit akun |
| DigitalOcean Droplet | Ya di semua region; DigitalOcean sendiri tidak merekomendasikan karena performa nested sering buruk | $6/bulan, ditagih per jam | Singapura (SGP1) | Tidak ada refund, tapi tagihan per jam sehingga uji beberapa jam hanya berbiaya sen |
| Contabo Cloud VDS | Ya. VDS adalah VM dengan core dan RAM dedicated, bukan bare metal | VDS S sekitar €49.40/bulan, VDS M €64.40, VDS L €91.60, sudah termasuk location fee Singapura | Singapura, Jepang, India, Australia | 14 hari money-back untuk akun pribadi, juga untuk perpanjangan otomatis dalam 72 jam terakhir; proses sampai 14 hari kerja |

Kandidat yang belum bisa dipastikan dari sumber primer: **WebHorizon**
(Singapura, Ryzen 9700X/9900X dan EPYC, dari $3/bulan) menulis "Nested
Virtualization Supported" di penawaran resminya, tetapi situs mereka
menolak diambil otomatis, jadi tanyakan dulu lewat tiket. **HostHatch**
(Singapura, Tokyo, Hong Kong) disebut pengguna mendukung nested tanpa
pernyataan resmi. **V.PS** (Singapura, Tokyo, Osaka, Qemu/KVM di atas
Proxmox VE) tidak menyebut nested sama sekali di FAQ-nya. **Bloom.host**
(Singapura, Ryzen 9 9950X, core dedicated) juga tidak menyebutnya.

## Cloud besar: nested resmi, dua di antaranya punya region Jakarta

Ini yang paling relevan kalau ingin servernya benar-benar di Indonesia:
dua cloud besar mendukung nested virtualization sebagai fitur resmi *dan*
punya region Jakarta, jadi latensinya beberapa milidetik, bukan 20-30 ms
ke Singapura. Harga per bulannya jauh di atas VPS kecil, tapi ditagih per
jam/detik, sehingga mengetes Nestbox setengah hari hanya beberapa ribu
rupiah.

| Provider | Cara mengaktifkan | Batasan penting | Region terdekat | Perkiraan biaya |
|---|---|---|---|---|
| Google Compute Engine | Set `enableNestedVirtualization` saat membuat VM, atau pada VM yang sudah ada; tanpa biaya tambahan | Bukan E2, bukan memory-optimized, bukan Arm, dan bukan AMD kecuali N4D; CPU Intel minimal Haswell; hypervisor di dalam VM hanya boleh KVM Linux (Hyper-V tidak didukung); performa CPU turun sekitar 10% atau lebih | **Jakarta (asia-southeast2)**, Singapura (asia-southeast1) | di Jakarta n1-standard-1 sekitar $32.64/bulan, n2-standard-2 sekitar $76.28/bulan on-demand; Spot jauh lebih murah untuk uji coba |
| Amazon EC2 | Sejak 12 Februari 2026 nested tersedia di instance biasa, bukan hanya bare metal: `--cpu-options "NestedVirtualization=enabled"` saat launch, atau ubah CPU options saat instance stopped; tanpa biaya tambahan | Hanya keluarga M7i/M8i, C7i/C8i, R7i/R8i/X8i, I7i/I7ie (varian -flex dan -d ikut); Graviton/Arm tidak; ketersediaan tipe berbeda per region | **Jakarta (ap-southeast-3)**, Singapura (ap-southeast-1) | tagihan per detik; cek dulu tipe mana yang ada di Jakarta dengan `aws ec2 describe-instance-types --region ap-southeast-3 --filters "Name=processor-info.supported-features,Values=nested-virtualization"` |
| Oracle Cloud (OCI) | Pakai shape VM Intel, misalnya `VM.Standard3.Flex`; `/dev/kvm` tersedia di dalam VM | Shape AMD dan Ampere (Arm) tidak mendukung nested, jadi Always Free tier tidak bisa dipakai untuk ini | Singapura (ap-singapore-1 / -2); tidak ada Jakarta | sekitar $0.04 per OCPU-jam ditambah biaya memori |
| Alibaba Cloud ECS | Nested aktif default, **tapi hanya di ECS Bare Metal** (keluarga `ebm`) | ECS VM biasa, termasuk tipe murah yang dijual di region Jakarta, tidak mengekspos vmx/svm | Jakarta (ap-southeast-5), Singapura | harga bare metal, jauh di atas VPS |

## Perbandingan spek: berapa replica yang muat, dan berapa harganya

Satu replica default memakai **4 vCPU, 4096 MB RAM, disk 20 GB thin**
(lihat `DISK=20G; RAM=4096; CPUS=4` di
[tools/linux/replica/appsandbox-replica](../tools/linux/replica/appsandbox-replica)),
ditambah base image Ubuntu sekitar 3,5 GB yang dipakai bersama semua
replica. vCPU boleh dijual berlebih — replica 4 vCPU tetap jalan di host
2 vCPU, hanya lebih lambat. Yang tidak bisa dilebihkan adalah **RAM dan
disk**, jadi dua angka itulah yang menentukan berapa replica yang muat:

- RAM: `(RAM host − 1 GB untuk host) ÷ 4 GB`
- Disk: `(disk host − 10 GB untuk OS host − 3,5 GB base image) ÷ 15 GB`
  (15 GB adalah pemakaian realistis satu replica dengan XFCE + Steam,
  di bawah batas 20 GB)

Kolom terakhir, **tenaga per replica**, adalah `(core ÷ jumlah replica) ×
(skor Geekbench 6 satu core ÷ 1000)`. Angka itu yang paling dekat dengan
"seberapa enak satu replica dipakai": desktop XFCE dan Steam menggambar
lewat CPU, dan yang menentukan bukan jumlah core host, melainkan berapa
core cepat yang tersisa untuk tiap replica. Nested sendiri sudah memotong
sekitar 10% menurut Google, dan replica di dalam VPS berarti dua lapis
virtualisasi.

| Plan | $/bulan | CPU (tahun) | GB6 1-core | Core | RAM / disk | Replica | $/replica | Tenaga per replica |
|---|---|---|---|---|---|---|---|---|
| OVHcloud VPS-1 | 4,54 | Haswell (2013) | ~848 | 2 | 4 GB / 40 GB | 1 (RAM diturunkan ~2,5 GB) | 4,54 | 1,7 |
| OVHcloud VPS-2 | 8,50 | Haswell (2013) | ~848 | 4 | 8 GB / 75 GB | 1 | 8,50 | 3,4 |
| OVHcloud VPS-3 | 12,32 | Haswell (2013) | ~848 | 6 | 12 GB / 100 GB | 2 | 6,16 | 2,5 |
| OVHcloud VPS-4 | 23,37 | Haswell (2013) | ~848 | 8 | 24 GB / 200 GB | 5 | **4,67** | 1,4 |
| GreenCloud SSDKVM-3 | 20 | EPYC Rome/Milan (2019-21) | ~1300 | 2 | 4 GB / 30 GB | 1 | 20 | 2,6 |
| GreenCloud SSDKVM-5 | 80 | EPYC Rome/Milan (2019-21) | ~1300 | 8 | 16 GB / 120 GB | 3 | 26,7 | 3,5 |
| GreenCloud promo tahunan | ~2,08 ($25/tahun) | EPYC Rome/Milan | ~1300 | 2 | 4 GB / 35 GB | 1 | **2,08** | 2,6 |
| **Onidel HF-4 (EPYC Turin, Singapura)** | 38,70 | **EPYC Turin Zen 5 (2024-25)** | **2976** | 6 | 12 GB / 120 GB, 6 TB | **2** | 19,35 | **8,9** |
| **Onidel HF-3 (EPYC Turin)** | 25,80 | EPYC Turin Zen 5 | 2976 | 4 | 8 GB / 80 GB, 4 TB | 1 (2 kalau RAM 3 GB) | 25,80 | **11,9** |
| Onidel HF-2 (EPYC Turin) | 12,90 | EPYC Turin Zen 5 | 2976 | 2 | 4 GB / 40 GB, 2 TB | 1 (RAM ~3 GB) | 12,90 | 5,95 |
| Onidel HF-5 (EPYC Turin) | 86,40 | EPYC Turin Zen 5 | 2976 | 8 | 32 GB / 240 GB, 8 TB | **7** | 12,34 | 3,4 |
| Onidel ONI-4 (Singapura) | 29,70 | EPYC 7713P Milan (2021) | ~1250 | 6 | 12 GB / 120 GB, 6 TB | **2** | 14,85 | 3,75 |
| Onidel ONI-3 (Singapura) | 19,80 | EPYC 7713P Milan (2021) | ~1250 | 4 | 8 GB / 80 GB, 4 TB | 1 (2 kalau RAM 3 GB) | 19,80 | 5,0 |
| Onidel ONI-5 (Singapura) | 65,60 | EPYC 7713P Milan (2021) | ~1250 | 8 | 32 GB / 240 GB, 8 TB | 7 | 9,37 | 1,4 |
| **Advin Servers EPYC Genoa 9654 (8 vCPU)** | 20 | **EPYC Genoa 9004 (2022-23)** | ~2400 | 8 | 16 GB / 256 GB | **3** | **6,67** | **6,4** |
| Advin Servers EPYC Genoa 9654 (2 vCPU) | 6 | EPYC Genoa 9004 (2022-23) | ~2400 | 2 | 4 GB / 64 GB | 1 (RAM ~3 GB) | 6 | 4,8 |
| Advin Singapura EPYC 9375F | 19,90 | EPYC Genoa 9004, 3,8 GHz (2023) | ~3000 | 2 | 8 GB / 40 GB | 1 | 19,90 | 6,0 |
| GreenCloud RyzenKVM-5 | 80 | **Ryzen 9950X (2024)** | ~3200 | 8 | 16 GB / 150 GB | 3 | 26,7 | 8,5 |
| GreenCloud RyzenKVM-4 | 40 | **Ryzen 9950X (2024)** | ~3200 | 4 | 8 GB / 80 GB | 1-2 | 20-40 | 6,4-12,8 |
| Cloudzy 4 GB | 14,48 | EPYC 9554 Genoa (2022) | ~2400 | 2 | 4 GB / 120 GB | 1 | 14,48 | 4,8 |
| **ExtraVM 8 GB** | 32 | **Ryzen 9 / EPYC 4004-4005 (2024-25)** | **~2500** | 4 | 8 GB / 120 GB | 1-2 | 16-32 | **10,0** (1 replica) |
| **ExtraVM 16 GB** | 56 | **Ryzen 9 / EPYC 4004-4005 (2024-25)** | **~2500** | 6 | 16 GB / 240 GB | **3** | 18,7 | **5,0** |
| SSD Nodes KVM/2X-LARGE | ~11,08 ($133/tahun) | Xeon Silver Skylake/Cascade (2017-19) | ~850 | 8 | 32 GB / 480 GB | **7** | **1,58** | 1,0 |
| DigitalOcean 8 GB | 48 | Intel/AMD campuran (2019-22) | ~1000-1300 | 4 | 8 GB / 160 GB | 1-2 | 24-48 | ~3,5 |
| Contabo Cloud VDS S | €42,99 (€34,40 kontrak setahun) | EPYC 7282 Rome (2019), core **fisik** | 1133 (terukur) | 3 fisik | 24 GB / 180 GB | **5** | ~€8 | 0,7-1,1 (tapi core-nya tidak dibagi tetangga) |
| GCE Jakarta n4-standard-2 | 89,03 + disk | **Emerald Rapids (2024)** | ~2000 | 2 | 8 GB / disk terpisah | 1-2 | 45-89 | 4,0 |
| GCE Jakarta n2-standard-2 | 76,28 + disk | Cascade/Ice Lake (2019-21) | ~900-1300 | 2 | 8 GB / disk terpisah | 1-2 | 40-76 | ~2,2 |
| AWS Jakarta m7i.large | ~75-90 + EBS | Sapphire Rapids (2023) | ~1050-1500 | 2 | 8 GB / EBS terpisah | 1-2 | 40-90 | ~2,5 |
| OCI Singapura VM.Standard3.Flex 1 OCPU | ~38 + volume | Ice Lake (2021) | ~1100 | 2 thread | 8 GB / terpisah | 1 | 38 | 2,2 |

Angka Geekbench 6 satu core diambil dari uji nyata: OVHcloud VPS-1 2027
terukur 848 dengan CPU yang dilaporkan sebagai "Intel Core Processor
(Haswell, no TSX)" — jadi lini VPS 2027 yang baru pun masih memakai
arsitektur 2013, sama seperti server yang sudah dicek. Contabo Cloud VDS M
terukur 1133 dengan EPYC 7282. Sisanya memakai rentang model CPU yang
sama di tabel Geekbench 6 VPSBenchmarks.

Indeks tenaga per replica mengandaikan semua replica sibuk bersamaan.
Kalau replica dipakai bergantian, plan dengan RAM besar dan core sedikit
(Contabo VDS S, SSD Nodes) jauh lebih baik daripada yang terlihat, dan
core fisik Contabo tidak pernah direbut tetangga — beda dengan vCore
shared di semua plan lain.

Dua hal lain yang tidak kelihatan di tabel:

- **Kuota traffic.** Layar replica lewat browser memakan sekitar 2-5
  Mbps. Dipakai 8 jam sehari selama sebulan itu 216 GB (2 Mbps) sampai
  540 GB (5 Mbps). Jadi OVH VPS-1 dengan kuota 500 GB bisa mepet lalu
  dicekik ke 10 Mbps; VPS-2/VPS-3 dengan 1 TB aman untuk pemakaian
  sedang; ExtraVM (4-10 TB) dan DigitalOcean (4-6 TB) lega.
- **Cara membayar.** SSD Nodes murah karena dibayar di muka untuk 1-3
  tahun: uangnya hangus kalau ternyata tidak cocok. GreenCloud promo
  tahunan juga tidak masuk kebijakan refund 7 hari mereka. Sebaliknya
  DigitalOcean, GCE, AWS, dan OCI ditagih per jam/detik, jadi risikonya
  hampir nol untuk menguji.

### Peringkat akhir

Kolom **nilai** adalah `tenaga per replica ÷ biaya per replica` — satu
angka yang sekaligus memuat harga, umur CPU, jumlah core, dan berapa
replica yang muat. Peringkatnya tidak murni mengikuti angka itu: yang
nilainya tinggi tapi menuntut bayar di muka bertahun-tahun, tidak bisa
refund, atau nested-nya belum dipastikan, diturunkan; yang stoknya selalu
ada dan nested-nya sudah terbukti, dinaikkan.

| # | Plan | $/bulan | CPU | Core | Replica | $/replica | Tenaga | Nilai |
|---|---|---|---|---|---|---|---|---|
| 1 | **Onidel HF-4, EPYC Turin Singapura** | 38,70 | **Turin Zen 5 (2024-25)** | 6 | **2** | 19,35 | **8,9** | **0,46** |
| 2 | **Onidel HF-2 / HF-3, EPYC Turin** | 12,90 / 25,80 | Turin Zen 5 | 2 / 4 | 1 | 12,90 / 25,80 | 5,95 / 11,9 | 0,46 |
| 3 | OVHcloud VPS-3 — stok selalu ada, nested terbukti sendiri | 12,32 | Haswell (2013) | 6 | 2 | 6,16 | 2,5 | 0,41 |
| 4 | Onidel HF-5 (kalau butuh banyak replica) | 86,40 | Turin Zen 5 | 8 | **7** | 12,34 | 3,4 | 0,28 |
| 5 | OVHcloud VPS-4 | 23,37 | Haswell (2013) | 8 | 5 | 4,67 | 1,4 | 0,29 |
| 6 | GreenCloud RyzenKVM-4 — Singapura DC2 **ada stok** | 40 | Ryzen 9950X (2024) | 4 | 1 | 40 | 12,8 | 0,32 |
| 7 | Cloudzy 4 GB | 14,48 | EPYC 9554 Genoa (2022) | 2 | 1 | 14,48 | 4,8 | 0,33 |
| 8 | Onidel ONI-3 / ONI-4 (Premium Milan) | 19,80 / 29,70 | EPYC 7713P Milan (2021) | 4 / 6 | 1 / 2 | 19,80 / 14,85 | 5,0 / 3,75 | 0,25 |
| 9 | GreenCloud promo tahunan | ~2,08 | EPYC Rome/Milan | 2 | 1 | 2,08 | 2,6 | 1,25 |
| 10 | SSD Nodes KVM/2X-LARGE | ~11,08 | Xeon Silver (2017-19) | 8 | 7 | 1,58 | 1,0 | 0,61 |
| 11 | Contabo Cloud VDS S | €42,99 | EPYC 7282 Rome (2019), core fisik | 3 fisik | 5 | ~€8 | 0,7-1,1 | ~0,07 |
| — | *Advin Servers EPYC Genoa 8 vCPU (nilai terbaik, tapi **habis** di Singapura dan Johor)* | 20 | Genoa (2022-23) | 8 | 3 | 6,67 | 6,4 | 0,96 |
| — | *Advin Servers EPYC Genoa 2 vCPU (**habis**)* | 6 | Genoa (2022-23) | 2 | 1 | 6,00 | 4,8 | 0,80 |
| — | *Advin Singapura EPYC 9375F (**habis**)* | 19,90 | Genoa 3,8 GHz (2023) | 2 | 1 | 19,90 | 6,0 | 0,30 |
| — | *ExtraVM 16 GB (**stok Singapura kosong**)* | 56 | Ryzen 9 / EPYC 4004-4005 | 6 | 3 | 18,7 | 5,0 | 0,27 |
| 12 | GCE Jakarta n4-standard-2 | 89 | Emerald Rapids (2024) | 2 | 1-2 | 45-89 | 4,0 | ~0,06 |
| 13 | DigitalOcean 8 GB | 48 | Intel/AMD campuran | 4 | 1-2 | 24-48 | 3,5 | ~0,10 |
| 14 | OCI Singapura VM.Standard3.Flex | ~38 | Ice Lake (2021) | 2 thread | 1 | 38 | 2,2 | 0,06 |
| 15 | AWS Jakarta m7i.large | ~80 | Sapphire Rapids (2023) | 2 | 1-2 | 40-90 | 2,5 | ~0,04 |
| 16 | GCE Jakarta n2-standard-2 | 76,28 | Cascade/Ice Lake | 2 | 1-2 | 40-76 | 2,2 | ~0,04 |

Baris yang ditulis miring dengan tanda "—" nilainya bagus tetapi
**stoknya kosong** saat dicek 5 September 2026, jadi tidak bisa dibeli
sekarang. Advin Servers tetap yang terbaik di atas kertas (nilai 0,96);
pasang "Get Notified" di plan yang diinginkan, stok dilepas ke publik
siapa cepat dia dapat.

**Peringkat 1-5 — bisa dibeli hari ini.** Lini **High Frequency Onidel
(EPYC Turin Zen 5, Singapura)** menang telak: nilainya 0,46, di atas
OVHcloud VPS-3 yang 0,41, padahal satu core-nya 3,5x lebih cepat (2976
berbanding 848 poin Geekbench 6). HF-4 seharga $38,70 memberi 2 replica
dengan tenaga 8,9 masing-masing — tertinggi di antara semua plan yang
muat lebih dari satu replica — plus kuota 6 TB, enam kali kuota OVHcloud
VPS-3. Nested aktif default di KVM Onidel (benchmark pihak ketiga di VPS
Singapura EPYC 7713P mencatat nested "Yes"), dan karena bisa ditagih per
jam, membuktikannya sendiri di HF-2 hanya $0,0192 per jam.

OVHcloud VPS-3 tetap di peringkat 3 karena masih yang termurah per
replica ($6,16) dan nested-nya sudah Anda buktikan sendiri. Lini Premium
Milan Onidel turun ke peringkat 8: HF-3 hanya 30% lebih mahal daripada
ONI-3 untuk satu core 2,4x lebih cepat, jadi tidak ada alasan memilih
Milan untuk replica desktop. GreenCloud Singapura DC2 juga terkonfirmasi
masih ada stok, baik lini Budget EPYC maupun Ryzen 9950X.

**Peringkat 6-11 — bagus untuk kebutuhan tertentu.** Cloudzy paling murah
di antara yang CPU-nya Genoa, dengan refund 14 hari sebagai jaring
pengaman karena nested-nya baru diklaim di halaman pemasaran. GreenCloud
promo tahunan punya nilai tertinggi di tabel tapi tidak bisa refund dan
stoknya musiman. SSD Nodes dan Contabo VDS S hanya masuk akal kalau
replica dipakai bergantian, bukan serentak.

**Peringkat 12-16 — hanya untuk alasan khusus:** server benar-benar di
Jakarta, atau tagihan per jam untuk uji coba beberapa jam. Untuk dipakai
24/7 semuanya kalah telak.

**Belum bisa dipastikan, tanyakan dulu:** WebHorizon (Ryzen 9700X/9900X
Singapura, menulis "Nested Virtualization Supported" di penawarannya),
HostHatch. **Tidak menyebut nested sama sekali:** V.PS, Bloom.host,
Kuroit, Melbicom, OneAsiaHost, Oplink (tidak punya lokasi Asia), PQ.Hosting
(baru buka Malaysia), Aeza (menyebut nested aktif, tetapi lokasinya
Eropa/Rusia, tanpa Asia Tenggara).

**Hindari untuk Nestbox:** semua plan dengan disk 15 GB (GreenCloud $6 dan
$10, ExtraVM $4,50) — base image 3,5 GB ditambah satu replica sudah
melewatinya. Juga GreenCloud SSDKVM-3 dan SSDKVM-5 dengan harga bulanan
normal: $20-26,7 per replica untuk CPU 2019-21 kalah dari Advin dan
OVHcloud di semua sumbu.

## VPS yang jelas tidak bisa

| Provider | Alasan |
|---|---|
| Hetzner Cloud | FAQ resmi: "nested virtualization is not possible on cloud server". Terbukti di server yang dicek |
| Contabo VPS biasa | Dokumentasi Contabo: nested hanya di VDS dan dedicated server |
| Vultr VPS | Tidak mendukung nested; hanya bare metal Vultr yang bisa |
| Linode / Akamai shared CPU | Ekstensi virtualisasi dinonaktifkan |
| Niagahoster | Terbukti di server yang dicek: tidak ada `/dev/kvm` |
| Hostinger VPS | Dokumentasi resmi: "Nested virtualization is not supported on Hostinger VPS hosting plans", pengguna diarahkan ke bare metal |
| Kamatera | FAQ resmi: "We currently do not support nested virtualization on our cloud servers" |

## Jakarta dan Malaysia

Untuk server yang benar-benar berada di Jakarta, jawaban yang pasti saat
ini adalah cloud besar: **Google Compute Engine region asia-southeast2**
dan **AWS ap-southeast-3**, keduanya dengan nested virtualization resmi
(lihat tabel di atas). Alibaba Cloud juga ada di Jakarta, tetapi nested-nya
hanya di bare metal.

Di kalangan VPS murah, belum ditemukan penyedia di Jakarta atau Malaysia
yang menyatakan nested virtualization secara eksplisit. Perwira Cloud
(Jakarta, Singapura) dan Shinjiru (Kuala Lumpur, Cyberjaya) menjual VPS
KVM tetapi tidak menyebut nested; Biznet Gio (NEO Lite / NEO Virtual
Compute) dan IDCloudHost juga tidak menyebutkannya di halaman produk.
Tanyakan langsung apakah VPS-nya mengekspos vmx/svm ke tamu. Provider
yang memakai panel SolusVM bisa mengaktifkan nested per VPS kalau
diminta, jadi menanyakan itu sering membuahkan hasil.

Alternatif yang sering sudah cukup: GreenCloudVPS Singapura DC2 terukur
13,68 ms dari Jakarta pada review di atas, praktis sama dengan server
lokal untuk layar replica lewat browser.

## Saran

- Urutan lengkap 16 plan ada di [Peringkat akhir](#peringkat-akhir) di
  atas. Tiga teratas: Advin Servers EPYC Genoa $20, Advin $6 untuk satu
  replica, lalu OVHcloud VPS-3 sebagai pilihan yang stoknya selalu ada.
- Latensi dari Indonesia ke Singapura sekitar 13-30 ms, cukup untuk
  layar replica lewat browser.
- Sisakan disk: satu replica memakai 20 GB (thin, tumbuh sesuai isi)
  ditambah base image Ubuntu sekitar 3,5 GB, dan desktop XFCE + Steam
  menambah beberapa GB lagi. Plan 15 GB tidak cukup.
- Ingat nested selalu lebih lambat: Google menyebut penurunan sekitar 10%
  atau lebih untuk beban yang CPU-bound.

## Sumber

- Spek dan harga yang dipakai di tabel perbandingan:
  [OVHcloud VPS Singapore](https://www.ovhcloud.com/asia/vps/vps-singapore/),
  [ExtraVM Singapore VPS](https://www.extravm.com/singapore-vps),
  [GreenCloud Budget KVM](https://greencloudvps.com/budget-kvm-vps.php),
  [SSD Nodes pricing](https://www.ssdnodes.com/pricing/),
  [DigitalOcean droplets](https://www.digitalocean.com/pricing/droplets),
  [Contabo Cloud VDS](https://onedollarvps.com/pricing/contabo-pricing),
  [m7i.large](https://instances.vantage.sh/aws/ec2/m7i.large)
- Angka CPU: [YABS OVHcloud VPS-1 2027 (Haswell, GB6 848)](https://www.vpsbenchmarks.com/yabs/ovhcloud-2c-4gb-20260831-fc8e52),
  [YABS Contabo Cloud VDS M (EPYC 7282, GB6 1133)](https://www.vpsbenchmarks.com/yabs/contabo-8c-31gb-20260611-tg10226),
  [daftar CPU menurut Geekbench 6 satu core](https://www.vpsbenchmarks.com/labs/cpus_by_geekbench6_perf),
  [OVH memindah lini VPS 2027 ke Intel lama](https://lowendtalk.com/discussion/218138/ovh-launches-2027-vps-line)
- Onidel: [Premium VPS](https://onidel.com/services/premium-vps),
  [High Frequency VPS EPYC Turin](https://onidel.com/services/high-frequency-vps),
  [benchmark Singapura yang mencatat nested "Yes"](https://lowendtalk.com/discussion/208589/onidel-singapore-vps-benchmark-and-review)
- Advin Servers: [Cloud VPS](https://advinservers.com/cloud),
  [halaman depan yang menyebut "KVM & Nested Virtualization"](https://advinservers.com/),
  [kebijakan refund 14 hari](https://docs.advinservers.com/policies/refund.md),
  [soal stok dan notifikasi restock](https://docs.advinservers.com/information/stock.md)
- Cloudzy: [Singapore VPS](https://cloudzy.com/singapore-vps/),
  [KVM VPS (EPYC 9554)](https://cloudzy.com/kvm-vps/),
  [nested virtualization](https://cloudzy.com/blog/vps-with-nested-virtualization/)
- GreenCloud lini Ryzen: [Ryzen KVM VPS](https://greencloudvps.com/ryzen-kvm-vps.php)
- V.PS: [FAQ](https://v.ps/faq/); Bloom.host: [Singapore VPS](https://bloom.host/singapore-vps/)
- ExtraVM: [Singapore VPS](https://www.extravm.com/singapore-vps),
  [nested virtualization](https://extravm.com/billing/knowledgebase/101/Is-nested-virtualization-enabled.html),
  [Terms of Service](https://extravm.com/tos.pdf)
- GreenCloud: [Budget KVM VPS](https://greencloudvps.com/budget-kvm-vps.php),
  [KVM VPS](https://greencloudvps.com/kvm-vps.php),
  [refund](https://green.cloud/docs/faq/can-i-get-a-refund/),
  review Indonesia dengan hasil nested dan latensi Jakarta:
  [GreenCloudVPS BudgetKVMSG-2](https://saidwp.com/blog/server/greencloudvps-budgetkvmsg2-review/)
- SSD Nodes: [Singapore](https://www.ssdnodes.com/singapore/),
  [nested virtualization VPS](https://www.ssdnodes.com/nested-virtualization-vps/),
  [Terms of Service](https://www.ssdnodes.com/SSD_Nodes_TOS.pdf)
- DigitalOcean: [KVM / nested virtualization](https://www.digitalocean.com/community/questions/does-digitalocean-support-kvm-or-nested-virtulzation),
  [refund](https://docs.digitalocean.com/support/can-i-have-a-refund/)
- Contabo: [nested virtualization](https://help.contabo.com/en/support/solutions/articles/103000271595-can-i-setup-nested-virtualization-on-my-server-),
  [location fees](https://contabo.com/en/location-fees/),
  [Singapore](https://contabo.com/blog/hello-from-singapore/),
  [refund](https://help.contabo.com/en/support/solutions/articles/103000327514-how-can-i-get-a-refund-)
- OVHcloud: [VPS Singapore](https://www.ovhcloud.com/asia/vps/vps-singapore/),
  [Starter VPS](https://www.ovhcloud.com/en-sg/vps/cheap-vps/),
  [managing orders / right of withdrawal](https://help.ovhcloud.com/csm/en-gb-billing-managing-ovh-orders?id=kb_article_view&sysparm_article=KB0042881)
- Google Cloud: [nested virtualization overview](https://docs.cloud.google.com/compute/docs/instances/nested-virtualization/overview),
  [enable nested virtualization](https://docs.cloud.google.com/compute/docs/instances/nested-virtualization/enabling),
  [tipe mesin dan harga di asia-southeast2](https://gcloud-compute.com/asia-southeast2.html)
- AWS: [nested virtualization di EC2](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/amazon-ec2-nested-virtualization.html),
  [region Jakarta](https://aws.amazon.com/blogs/aws/now-open-aws-asia-pacific-jakarta-region)
- Oracle Cloud: [KVM nested virtualization di OCI](https://blogs.oracle.com/linux/kvm-nested-virtualization-in-oci)
- Alibaba Cloud: [ECS Bare Metal](https://www.alibabacloud.com/help/en/ecs/user-guide/elastic-bare-metal-server-overview)
- Hetzner Cloud: [FAQ](https://docs.hetzner.com/cloud/servers/faq/)
- Hostinger: [Is nested virtualization supported?](https://www.hostinger.com/support/10429687-is-nested-virtualization-supported)
- Kamatera: [Infrastructure & Networking FAQ](https://www.kamatera.com/faq/infrastructure-and-networking/)
- Vultr: [VT-x / AMD-V](https://discuss.vultr.com/discussion/920/vt-x-amd-v-on-a-vultr-server)
- Linode: [nested virtualization](https://www.linode.com/community/questions/19459/do-any-linode-regionsinstances-support-nested-vmvirtualization)
- Perwira Cloud: [VPS Linux KVM](https://perwiracloud.com/vps-linux-kvm);
  Shinjiru: [KVM VPS](https://www.shinjiru.com.my/enterprise/ssd-virtual-private-server-linux-kvm/);
  Biznet Gio: [NEO Lite](https://www.biznetgio.com/product/neo-lite)
- WebHorizon: [virtual servers](https://webhorizon.net/virtual-server.html);
  HostHatch: [diskusi nested di LowEndTalk](https://lowendtalk.com/discussion/182484/how-good-is-nested-virt-on-the-providers-here)
- SolusVM: [enable nested virtualization](https://support.solusvm.com/hc/en-us/articles/13267974631447-How-to-enable-the-Nested-virtualization-for-KVM-VPS-in-SolusVM)
