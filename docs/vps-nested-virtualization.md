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

Data di bawah dikumpulkan 5 September 2026 (sapuan pertama) dan 7
September 2026 (sapuan kedua, 60-an brand lain): hasil cek langsung ke
server yang ada, dan dokumentasi resmi provider (tautan di bagian
akhir). Harga dan kebijakan bisa berubah; cek ulang sebelum membeli.

## Ringkasan

Peringkat lengkap 16 plan ada di [Peringkat akhir](#peringkat-akhir);
rumus ukurannya di [Menerjemahkan beban](#menerjemahkan-beban-yang-mau-dijalankan-ke-ukuran-vps).
Intinya, per 7 September 2026:

| # | Plan | $/bulan | Core / RAM | Replica | Kenapa |
|---|---|---|---|---|---|
| 1 | **Onidel HF-4, EPYC Turin, Singapura** | 38,70 | 6 / 12 GB | 2 | Core tercepat (Geekbench 6 satu core 2976), nested resmi di KB, refund 14 hari, tagihan per jam. **Stok sering habis** |
| 2 | Onidel HF-2 / HF-3 | 12,90 / 25,80 | 2 / 4 GB, 4 / 8 GB | 1 | Sama seperti di atas, untuk satu replica |
| 3 | **OVHcloud VPS-3** | 12,32 | 6 / 12 GB | 2 | Termurah per replica, nested terbukti sendiri, stok selalu ada. Kelemahan: Haswell 2013 |
| 4 | Onidel HF-5 | 86,40 | 8 / 32 GB | 7 | Kalau butuh banyak replica |
| 5 | OVHcloud VPS-4 | 23,37 | 8 / 24 GB | 5 | Banyak replica dengan biaya terkecil |
| 6 | GreenCloud RyzenKVM-4, Singapura DC2 | 40 | 4 / 8 GB | 1 | Ryzen 9950X, stok ada, nested terbukti pengguna |
| — | Advin Servers EPYC Genoa 8 vCPU | 20 | 8 / 16 GB | 3 | Nilai terbaik di atas kertas, tapi **habis**; pasang "Get Notified" |

Enam replica headless butuh **minimal 12 GB RAM** (6 GB dan 8 GB tidak
cukup); yang paling pas: Onidel HF-4 atau OVHcloud VPS-3. Cloud besar
(GCE, AWS, OCI) resmi mendukung nested dan punya ukuran 12-16 GB, tetapi
$76-117 per bulan untuk 2 vCPU. Sapuan 60-an brand lain tidak menemukan
pesaing baru yang nested-nya resmi dan berlokasi di Asia; cadangannya
V.PS Osaka Edge dan DMIT Hong Kong (bukti pengguna), dan untuk Jakarta
uji LightNode per jam dulu.

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
| **Onidel Cloud** | **Ya, ada artikel KB resminya**: nested virtualization didukung di VPS High-Performance dan High-Frequency mereka, di semua lokasi, tanpa perlu minta diaktifkan. Dua nama di KB itu persis dua lini yang dijual: "AMD EPYC Premium" bertagline *high-performance*, dan "High Frequency / EPYC Turbo" yang Turin. Benchmark pihak ketiga yang mencatat nested "Yes" justru dilakukan di **lini Premium** (EPYC 7713P Singapura), jadi lini itulah yang buktinya paling kuat. Catatan: storage Premium adalah NVMe terdistribusi tiga salinan (ada failover otomatis, tetapi latensi disknya di atas NVMe lokal) | Premium EPYC Milan, harga Singapura per bulan (juga bisa per jam): ONI-1 1 core / 2 GB / 20 GB / 1 TB $4,95; ONI-2 2 / 4 GB / 40 GB / 2 TB $9,90; ONI-3 4 / 8 GB / 80 GB / 4 TB $19,80; ONI-4 6 / 12 GB / 120 GB / 6 TB $29,70; ONI-5 8 / 32 GB / 240 GB / 8 TB $65,60; ONI-6 12 / 48 GB / 320 GB / 12 TB $95. Diskon 5-30% untuk kontrak 3 bulan sampai 3 tahun. **High Frequency EPYC Turin (Zen 5, 3,8+ GHz, Geekbench 6 satu core 2976 menurut Onidel)**, Singapura saja: HF-1 1 core / 2 GB / 20 GB / 1 TB $6,45; HF-2 2 / 4 GB / 40 GB / 2 TB $12,90; HF-3 4 / 8 GB / 80 GB / 4 TB $25,80; HF-4 6 / 12 GB / 120 GB / 6 TB $38,70; HF-5 8 / 32 GB / 240 GB / 8 TB $86,40; HF-6 12 / 48 GB / 320 GB / 12 TB $126,20. Tagihan per jam tersedia (HF-2 $0,0192/jam, HF-4 $0,0576/jam) | Singapura (Turin hanya di sini), Sydney, Melbourne, Ho Chi Minh, Amsterdam, New York | ToS: **14 hari money-back penuh**. Tapi FAQ di situsnya menyebut 7 hari dan hanya kalau layanannya rusak/tidak sesuai — ToS yang mengikat, siapkan tautannya kalau ditanya. Pengecualian: pemakaian CPU/bandwidth tinggi sebelum minta refund, pelanggaran AUP. **Plan promo (kode diskon atau sale) hanya di-refund jadi kredit akun**, dan **top-up saldo tidak bisa di-refund sama sekali** |
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

## Sapuan kedua, 7 September 2026: brand lain

Empat puluh lebih provider lain diperiksa dengan cara yang sama (situs
resmi, ToS, halaman order, dan output YABS pengguna; baris
`VM-x/AMD-V: Enabled` di YABS membuktikan flag vmx/svm sampai ke guest).
Hasil kerasnya: **tidak satu pun brand baru yang menulis nested "ya"
secara resmi *dan* punya lokasi Asia**. Yang ada adalah bukti pengguna,
kebijakan yang tidak melarang, atau lokasi yang tepat tanpa pernyataan.
Tabel pertama adalah yang layak dicoba, dengan syarat tanya tiket dulu
atau uji `grep -c vmx /proc/cpuinfo` di masa refund.

| Provider | Bukti nested | Lokasi Asia | Harga yang relevan | Refund | Catatan |
|---|---|---|---|---|---|
| **V.PS (xTom)** | Pengguna: YABS Tokyo Performance, EPYC 7763, `VM-x/AMD-V: Enabled`, Geekbench 6 satu core 1653. FAQ 45 butir tidak menyebut nested | Singapura, Tokyo, Osaka, Hong Kong, Sydney | Singapura mahal: Performance 2 core / 2 GB / 30 GB €46,95, 8 core / 16 GB / 240 GB €329,95; Edge Singapura hanya 1 core / 2 GB €15,95. Yang masuk akal **Osaka Edge**: 4 core / 8 GB / 80 GB / 4 TB €35,95; 8 core / 16 GB / 160 GB / 8 TB €65,95. Tokyo Cloud dari €6,95 | 14 hari untuk VPS; sekali per akun; batal kalau trafik >10 GB; kripto, transfer bank, renewal, flash sale tidak refund | KVM di atas Proxmox VE, jaringan xTom AS8888. Halaman cart di balik Cloudflare, stok tidak bisa dicek |
| **DMIT** | Pengguna: review LAX Pro dan HKG mencatat `VirtReady: Yes (Nested Virtualization)`, EPYC 7402P. **AUP resmi hanya melarang "secondary virtualization for commercial purposes"**, jadi pemakaian sendiri tidak dilarang | Hong Kong (Equinix HK2), Tokyo; **tidak ada Singapura** | HKG.AS3.T1: 4 vCPU / 4 GB / 80 GB / 16 TB $32,90; 4 vCPU / 8 GB / 160 GB / 32 TB $49,90; 8 vCPU / 16 GB / 320 GB / 64 TB $99,90. Tokyo harga sama tapi sering sold out | Penuh ≤3 hari (order baru, trafik ≤30 GB, dipotong fee gateway); pro-rata ≤30 hari; maks 3x per seri | EPYC 7402P Geekbench 6 satu core 851; AUP hanya menjamin 50% CPU. Stok HKG ada, TYO habis |
| **UpCloud** | **Mati default**: YABS Singapura General Purpose `VM-x/AMD-V: Disabled`. Satu komentar LowEndTalk 2026 menyebut support bisa mengaktifkan nested; belum terverifikasi | Singapura (Equinix SIN1), Sydney | Starter EPYC 7542: 4 vCPU / 8 GB / 40 GB €20; 4 vCPU / 16 GB / 50 GB €28. Premium EPYC 9575F (Zen 5): 4 vCPU / 8 GB / 100 GB €52, 8 vCPU / 16 GB / 200 GB €148. Tagihan per jam, egress gratis | 30 hari money-back untuk pembayaran pertama (maks €500); trial gratis 7 hari | Disk Starter tipis (40-50 GB). Geekbench 6 satu core 1175 (Starter), ~3000 (Premium). Pakai trial untuk tanya dan uji |
| **LightNode** | Campuran per node: YABS Dubai `Enabled` (Haswell), YABS Hong Kong `Disabled` (CPU "QEMU Virtual"). Materi pemasaran lini **VDS** menulis "run Docker, KVM-based nested virtualization"; halaman VPS tidak menyebutnya | **Jakarta**, Singapura, Kuala Lumpur, Tokyo, Hong Kong, Bangkok, Hanoi, Manila, dan belasan kota Asia lain | 2 vCPU / 4 GB / 50 GB / 2 TB $14,70; 4 vCPU / 8 GB / 50 GB / 3 TB $27,70; 8 vCPU / 16 GB / 50-150 GB / 4 TB $52,70-54,11; tagihan per jam, top-up minimum $10 | ToS: **semua pembayaran tidak dapat dikembalikan** | Satu-satunya VPS murah dengan Jakarta. Port 100 Mbps, disk 50 GB, CPU Xeon lama (Geekbench 6 satu core ~640-1190). Deploy satu jam, cek `/proc/cpuinfo`, hancurkan kalau tidak ada vmx |
| MassiveGRID | Pengguna: YABS VDS New York dan Frankfurt `VM-x/AMD-V: Enabled`; blog resmi: "hardware virtualization extensions are available to guest VMs". Tapi VPS promo murah mencatat "Nested Virt.? No" | Singapura (Equinix), New York, London, Frankfurt | VDS Starter 2 vCPU / 4 GB / 64 GB $14,99; Growth 4 / 8 GB / 128 GB $29,99; Professional 8 / 16 GB / 256 GB $59,99 | 15 hari money-back | Xeon Gold 6130 / E5-2683 v4 (Geekbench 6 satu core 650-790). Reputasi LowEndTalk buruk: downtime, CPU steal tinggi. Hanya untuk uji coba di masa refund |
| Virtono | Pengguna: YABS Tokyo dan Milan `VM-x/AMD-V: Enabled`, Xeon Gold 5218. Tidak ada pernyataan resmi | Singapura, Tokyo, Hong Kong | KVM 4G 2 vCPU / 4 GB / 80 GB / 3 TB €20,95; 8G 4 / 8 GB / 160 GB €42,95; 16G 8 / 16 GB / 320 GB €89,95 | ToS: hak pembatalan 14 hari **gugur begitu layanan disediakan** (kecuali web hosting); ada keluhan refund ditolak | Geekbench 6 satu core 940; harga 2-4x pesaing. Hanya menarik saat flash sale |

Punya Singapura, tetapi tidak menyebut nested sama sekali di situs, KB,
maupun ToS (tanya tiket sebelum bayar; sebagian tanpa refund):

| Provider | Lokasi | CPU dan harga | Refund |
|---|---|---|---|
| LayerStack | Singapura, Hong Kong, Tokyo, Taipei | Xeon Scalable / EPYC Genoa; R208 4 vCPU / 12 GiB / 250 GiB $34,49; R308 8 / 16 GiB / 350 GiB $50; PRO300 EPYC 9745 Turin 8 vCPU / 16 GB / 300 GB $64 (tarif kontrak; bulanan lebih mahal). Trafik unlimited | **Tidak ada**; ada trial gratis |
| HostUS | Singapura, Sydney (Hong Kong menyusul) | KVM-4 4 core / 4 GB / 90 GB / 4 TB $22,95; KVM-8 6 / 8 GB / 150 GB $49,95; KVM-12 8 / 12 GB / 200 GB $69,95; KVM-16 8 / 16 GB / 250 GB $89,95. ToS hanya melarang nested di OpenVZ, jadi di KVM secara kebijakan boleh | 3 hari untuk pelanggan baru, gugur kalau >10% kuota terpakai |
| Regxa | Singapura, Mumbai, Sydney | Klaim EPYC 9354; x3 2 vCPU / 4 GB / 120 GB $15; x4 4 / 8 GB / 240 GB $30; x5 8 / 16 GB / 350 GB $69,99. Kuota trafik Asia tidak dipublikasikan | Pro-rata ke kredit akun saja |
| Vodien | Singapura (perusahaan lokal) | Klaim EPYC 9004; NVME 200 4 vCPU / 8 GB / 200 GB / 4 TB S$34,77 (~$27); NVME 300 4 / 12 GB / 300 GB S$49,68 (~$39); NVME 400 6 / 16 GB / 400 GB S$69,54 (~$54). **Port 200 Mbps** | Pro-rata dalam 30 hari pertama |
| BigCloudy | Singapura | "Intel"; Business 4 vCPU / 6 GB / 100 GB $14,99; Premium 6 / 12 GB / 200 GB $20,99 (harga tahunan) | 30 hari menurut pihak ketiga, tidak terverifikasi |
| Serverwala | Singapura | 4 vCPU / 4 GB / 100 GB / 500 GB $40; 6 / 6 GB / 150 GB $50 | 7 hari, kredit saja |
| Evolution Host | Singapura (harga 2x lokasi lain, **port 200 Mbps**) | 4 vCPU / 4 GB / 80 GB €80; 8 / 8 GB / 120 GB €120; 12 / 12 GB / 160 GB €160 di Singapura | Refund jadi saldo akun |
| ZgoVPS | Hong Kong, Tokyo, Osaka (tidak ada Singapura) | Osaka EPYC 9354P: 3 vCPU / 4 GB / 80 GB $128/tahun; 6 vCPU / 8 GB / 120 GB $198/tahun. Bukti nested hanya di LA. Tidak ada plan 12-16 GB di Asia | ≤3 hari, trafik <10 GB; plan Osaka **tidak bisa refund** |
| Vmiss | Hong Kong, Tokyo, Osaka, Seoul | Tokyo TRI 4 vCPU / 8 GB / 80 GB CAD 120 (~$88); tier bawah disk 10-20 GB. Bukti nested hanya di LA | tidak ditemukan |
| Hyonix | Singapura, Tokyo | Hyper-V dan **Windows saja**, tidak cocok untuk guest Linux | 7 hari |
| Servers.com | Singapura, Hong Kong | Cloud OpenStack/KVM, harga lewat sales | — |

Yang terbukti mengekspos nested tetapi tidak punya lokasi Asia, dicatat
supaya tidak dicek ulang: **Webdock** (Denmark; resmi "supported on all
Webdock KVM servers"), **Katapult / Krystal** (Amsterdam, London, New
York, Phoenix; YABS `Enabled`, EPYC Milan, refund 60 hari), **Terabit**
(AS; YABS `Enabled`, EPYC 7742, 4 vCPU / 16 GB $14,99), **Servarica**
(Montreal; resmi "supports nested virtualization" di lini KVM V2/V3,
tetapi stok 0), **HostEONS** (AS/Eropa; YABS `Enabled` di VDS Ryzen),
**AlphaVPS** (Eropa/AS; ambigu), **Crunchbits** yang kini bernama Synteq
HPC (AS/Bulgaria; FAQ lama "enabled by default", dokumen baru tidak
menyebut), dan **HostKey** (Eropa). WebHorizon tetap satu-satunya offer
Asia di LowEndTalk yang menulis nested secara eksplisit.

Tambahan untuk daftar "jelas tidak bisa" (semua punya lokasi Asia, itu
sebabnya sering muncul di pencarian):

| Provider | Alasan |
|---|---|
| Netcup | Singapura dibuka Desember 2025, tetapi FAQ resmi: "the SVM flag is therefore disabled on our systems and cannot be activated" |
| Leaseweb VPS | Singapura dan Tokyo, 6 vCPU / 16 GB hanya €8,99, tetapi YABS Singapura Januari 2025, November 2025, dan Agustus 2026 semuanya `VM-x/AMD-V: Disabled` |
| BandwagonHost | ToS melarang "Nested virtualization (e.g. running Qemu)"; YABS Hong Kong dan Singapura `Disabled` |
| RackNerd | ToS melarang nested "unless expressly approved in writing"; KVM VPS-nya pun tidak dijual di Singapura |
| Hostwinds | Tutorial resmi: "on our VPSs, it cannot be supported"; tidak ada Asia |
| Alibaba, Tencent, Huawei Cloud | Jakarta ada, nested hanya di bare metal |

## Cloud besar: nested resmi, dua di antaranya punya region Jakarta

Ini yang paling relevan kalau ingin servernya benar-benar di Indonesia:
dua cloud besar mendukung nested virtualization sebagai fitur resmi *dan*
punya region Jakarta, jadi latensinya beberapa milidetik, bukan 20-30 ms
ke Singapura. Harga per bulannya jauh di atas VPS kecil, tapi ditagih per
jam/detik, sehingga mengetes Nestbox setengah hari hanya beberapa ribu
rupiah.

| Provider | Cara mengaktifkan | Batasan penting | Region terdekat | Perkiraan biaya |
|---|---|---|---|---|
| Google Compute Engine | Set `enableNestedVirtualization` saat membuat VM, atau pada VM yang sudah ada; tanpa biaya tambahan | Bukan E2, bukan memory-optimized, bukan Arm, dan bukan AMD kecuali N4D; CPU Intel minimal Haswell; hypervisor di dalam VM hanya boleh KVM Linux (Hyper-V tidak didukung); performa CPU turun sekitar 10% atau lebih | **Jakarta (asia-southeast2)**, Singapura (asia-southeast1) | di Jakarta n1-standard-1 sekitar $32.64/bulan, n2-standard-2 sekitar $76.28/bulan, n4-highmem-2 (2 vCPU / 16 GB) $116.83/bulan on-demand; Spot jauh lebih murah untuk uji coba (n4-highmem-2 ~$34) |
| Amazon EC2 | Sejak 12 Februari 2026 nested tersedia di instance biasa, bukan hanya bare metal: `--cpu-options "NestedVirtualization=enabled"` saat launch, atau ubah CPU options saat instance stopped; tanpa biaya tambahan | Hanya keluarga M7i/M8i, C7i/C8i, R7i/R8i/X8i, I7i/I7ie (varian -flex dan -d ikut); Graviton/Arm tidak; ketersediaan tipe berbeda per region | **Jakarta (ap-southeast-3)**, Singapura (ap-southeast-1) | tagihan per detik; di Jakarta m7i.large (2 vCPU / 8 GB) $0.126/jam ≈ $92/bulan, r7i.large (2 vCPU / 16 GB) $0.160/jam ≈ $117/bulan; cek dulu tipe mana yang ada di Jakarta dengan `aws ec2 describe-instance-types --region ap-southeast-3 --filters "Name=processor-info.supported-features,Values=nested-virtualization"` |
| Oracle Cloud (OCI) | Pakai shape VM Intel, misalnya `VM.Standard3.Flex`; `/dev/kvm` tersedia di dalam VM | Shape AMD dan Ampere (Arm) tidak mendukung nested, jadi Always Free tier tidak bisa dipakai untuk ini | Singapura (ap-singapore-1 / -2); tidak ada Jakarta | $0.04 per OCPU-jam + $0.0015 per GB-jam: 1 OCPU / 12 GB ≈ $42/bulan, 2 OCPU / 16 GB ≈ $76/bulan |
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

### Menerjemahkan beban yang mau dijalankan ke ukuran VPS

Kalau sudah tahu apa yang akan berjalan di dalam replica, ukurannya bisa
dihitung, tidak perlu ditebak. Ambil contoh nyata: **6 replica Ubuntu,
masing-masing menjalankan satu proses yang memakai 300 MB RAM dan 5% CPU
di Ryzen 7 5800H** (8 core / 16 thread, Geekbench 6 satu core sekitar
2000).

**RAM** — ini yang hampir selalu mengikat:

```
RAM per replica = proses + OS Ubuntu + ruang cache
                = 300 MB + ~400 MB + sisanya  →  1536 MB batas bawah,
                                                 2048 MB kalau pakai XFCE
RAM host        = (RAM per replica + ~150 MB overhead QEMU) x jumlah replica
                  + ~1 GB untuk OS host + Nestbox
```

Jangan tergoda menurunkan replica ke 1024 MB supaya muat di plan 8 GB:
setelah kernel, systemd, dan proses 300 MB itu, yang tersisa untuk page
cache tinggal 200-300 MB, replica mulai swap, dan proses yang tadinya
memakai 5% CPU jadi menunggu disk. Pengalaman di lapangan sama: **6 GB
tidak cukup, 8 GB pun tidak cukup** untuk enam replica.

Enam replica headless di 1536 MB berarti `6 x 1686 MB + 1 GB ≈ 11 GB`,
jadi **minimum praktisnya 10-12 GB**, dan plan 12 GB adalah ukuran
terkecil yang dijual di angka itu. Kalau keenamnya memakai desktop XFCE,
angkanya naik ke sekitar 13,5 GB dan perlu plan 16 GB atau lebih.

**CPU** — perhatikan dulu 5% itu diukur dari mana, karena bedanya 16 kali:

- Task Manager Windows menghitung 5% dari **seluruh** chip: di 5800H itu
  `0,05 x 16 thread = 0,8 thread` per proses, jadi 6 proses = 4,8 thread.
- `top` di Linux menghitung 100% = **satu** core: 5% berarti 0,05 core per
  proses, jadi 6 proses hanya 0,3 core.

Setelah itu, konversikan ke CPU tujuan dengan perbandingan skor satu core,
tambahkan idle OS tiap replica (~0,05 core), tambah 15% untuk nested, lalu
tambah ~0,4 core untuk host dan thread I/O QEMU:

```
core = (beban x skor_asal/skor_tujuan + 0,05 x replica) x 1,15 + 0,4
```

Dengan pembacaan Task Manager dan tujuan EPYC Turin (skor 2976):
`(4,8 x 2000/2976 + 0,3) x 1,15 + 0,4 ≈ 4,4 core` — jadi 6 core cukup dan
4 core mepet. Dengan pembacaan `top`, hasilnya kurang dari 1 core dan
CPU-nya tidak jadi soal sama sekali.

**Disk** — tiap replica adalah qcow2 dengan base image sebagai backing
file, jadi base 3,5 GB itu dibayar sekali untuk semua replica, dan tiap
replica hanya menyimpan perubahannya sendiri (beberapa GB untuk Ubuntu
headless). Enam replica headless butuh sekitar `3,5 + 6 x 3 + 10 GB OS
host ≈ 32 GB`.

Kesimpulan contoh ini bergantung pada pembacaan tadi, dan bedanya besar:

- Kalau 5% itu bacaan `top` (0,05 core per proses), semua plan 12 GB cukup
  — termasuk **OVHcloud VPS-3 $12,32**, yang mengikat cuma RAM.
- Kalau 5% itu bacaan Task Manager (0,8 thread per proses), kebutuhannya
  jadi `4,8 thread 5800H`, dan itu berarti **4,2 core Ryzen 9950X** atau
  **4,4 core EPYC Turin**, tetapi **11,3 core Haswell** — jauh di atas 6
  atau 8 vCPU yang dijual OVHcloud, dan juga di atas 8 core EPYC Milan.
  Dengan beban seberat itu hanya CPU cepat yang masuk akal: GreenCloud
  Ryzen 9950X 8 core / 16 GB ($80) atau Onidel HF-4 kalau stoknya kembali.
- Kalau harus cloud besar (server di Jakarta, atau butuh tagihan per
  jam), ukuran 12-16 GB-nya ada, hanya saja 3-10x lebih mahal daripada
  VPS dengan RAM sama. Tipe termurah yang mendukung nested per RAM-nya:

  | Cloud | Tipe (nested resmi) | vCPU / RAM | $/bulan on-demand | Spot/preemptible |
  |---|---|---|---|---|
  | GCE Jakarta | n4-highmem-2 (Emerald Rapids) | 2 / 16 GB | 116,83 + disk | ~34 ($0,0464/jam) |
  | GCE Jakarta | n2-highmem-2 (Cascade/Ice Lake) | 2 / 16 GB | 102,90 + disk | ~77 |
  | GCE Jakarta | n4-standard-4 | 4 / 16 GB | 178,07 + disk | ~52 |
  | GCE Jakarta | n4-custom-2-12288 (2 vCPU / 12 GB, custom) | 2 / 12 GB | ~105 + disk | — |
  | AWS Jakarta | r7i.large (Sapphire Rapids) | 2 / 16 GB | ~117 ($0,160/jam) + EBS | tidak tercatat di Jakarta |
  | AWS Jakarta | m7i-flex.xlarge | 4 / 16 GB | ~174 ($0,239/jam) + EBS | ~25 ($0,034/jam) |
  | AWS Jakarta | m7i.xlarge | 4 / 16 GB | ~184 ($0,252/jam) + EBS | — |
  | OCI Singapura | VM.Standard3.Flex 1 OCPU / 12 GB | 2 thread / 12 GB | ~42 + volume | — |
  | OCI Singapura | VM.Standard3.Flex 2 OCPU / 16 GB | 4 thread / 16 GB | ~76 + volume | — |

  Disk dihitung terpisah: persistent disk GCE dan EBS gp3 sekitar $0,10
  per GB per bulan di Jakarta, jadi 40 GB menambah ~$4; block volume OCI
  sekitar $0,0255 per GB. Singapura di GCE sekitar 8% lebih murah
  daripada Jakarta untuk tipe yang sama, di AWS harganya sama. OCI
  paling murah di antara cloud besar karena RAM dihargai $0,0015 per
  GB-jam, tetapi hanya ada Singapura, dan shape Intel-nya wajib (AMD
  E-series dan Ampere tidak mengekspos KVM). Semua opsi ini cukup untuk
  pembacaan `top` (CPU-nya tidak jadi soal); untuk pembacaan Task
  Manager perlu 6-7 vCPU Emerald Rapids, artinya n4-standard-8 (8 vCPU /
  32 GB, ~$356/bulan) — jauh di atas GreenCloud Ryzen $80.

Cara memastikannya di mesin 5800H: jalankan `top` lalu lihat kolom %CPU
proses itu (100% = satu core), atau di Task Manager buka tab Details dan
bandingkan dengan tab Performance — kalau Task Manager menulis 5% untuk
satu proses, itu 5% dari seluruh 16 thread. Tiap replica dibuat dengan `--cpus 2 --ram 1536 --disk 20G`; vCPU
boleh dijual berlebih (6 x 2 = 12 vCPU di 6 core) karena bebannya
sebentar-sebentar.

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
| GCE Jakarta n4-highmem-2 | 116,83 + disk | Emerald Rapids (2024) | ~2000 | 2 | 16 GB / disk terpisah | 3 | 39 | 1,3 |
| AWS Jakarta r7i.large | ~117 + EBS | Sapphire Rapids (2023) | ~1050-1500 | 2 | 16 GB / EBS terpisah | 3 | 39 | ~0,8 |
| OCI Singapura VM.Standard3.Flex 2 OCPU / 16 GB | ~76 + volume | Ice Lake (2021) | ~1100 | 4 thread | 16 GB / terpisah | 3 | 25 | 1,5 |
| V.PS Osaka Edge Orange (nested: bukti pengguna) | €65,95 (~72) | EPYC 7763 Milan (2021) | 1653 (terukur, Tokyo) | 8 | 16 GB / 160 GB, 8 TB | 3 | 24 | 4,4 |
| V.PS Osaka Edge Yellow (nested: bukti pengguna) | €35,95 (~39) | EPYC 7763 Milan (2021) | 1653 | 4 | 8 GB / 80 GB, 4 TB | 1 (2 kalau RAM 3 GB) | 39 | 6,6 |
| DMIT HKG.AS3.T1 MEDIUM (nested: bukti pengguna) | 49,90 | EPYC 7402P Rome (2019) | 851 (terukur) | 4 | 8 GB / 160 GB, 32 TB | 1 | 49,90 | 3,4 (AUP hanya menjamin 50% CPU) |
| UpCloud Singapura Starter 4 / 16 GB (nested: mati default, tanya support) | €28 (~30) | EPYC 7542 Rome (2019) | 1175 (terukur) | 4 | 16 GB / 50 GB | 2 (disk yang membatasi) | 15 | 2,4 |

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

### Stok yang terverifikasi, 5 September 2026

Stok berubah cepat dan itu yang paling sering menentukan pilihan, bukan
harga. Hasil cek langsung ke halaman order:

| Provider | Stok Singapura |
|---|---|
| **GreenCloud Ryzen KVM, Singapura DC2** | **Semua tier tersedia**: 1 GB $6, 2 GB $10, 4 GB / 2 core $20, 8 GB / 4 core $40, **16 GB / 8 core / 150 GB $80** |
| GreenCloud Budget KVM (EPYC), Singapura DC1 & DC2 | Tersedia |
| OVHcloud | Selalu tersedia |
| ExtraVM | Tinggal 1 GB (8 unit) dan 2 GB (1 unit); 3 GB ke atas **habis semua** |
| Advin Servers (Singapura dan Johor) | **Habis** |
| Onidel High Frequency Turin | **Habis** — order ditolak dengan "Resources not available" |
| Cloudzy, DigitalOcean, Contabo VDS, SSD Nodes, cloud besar | Tersedia (kapasitas besar, jarang kosong) |

GreenCloud juga menjual tambahan satuan: $3 per GB RAM, $3 per core, $3
per 10 GB disk. Jadi RyzenKVMSG2-4 ($40, 4 core, 8 GB) ditambah 4 GB RAM
dan 2 core menjadi 6 core / 12 GB seharga $58 — lebih murah daripada naik
ke tier 16 GB kalau yang dibutuhkan cuma 12 GB.

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

Cara memakai kebijakan Onidel tanpa terjebak: beli HF bulanan **tanpa
kode promo** kalau ingin hak refund 14 hari yang bisa kembali ke kartu —
memakai kode promo Turin (40% tahunan / 60% triennial) membuat refund
hanya keluar sebagai kredit akun. Tagihan per jam dihitung dari harga
bulanan dibagi 672 dan tidak pernah melebihi harga bulanan, tetapi
diambil dari saldo yang di-top-up lebih dulu, dan **top-up tidak bisa
di-refund**, jadi isi seperlunya saja. VM yang cuma di-stop tetap
ditagih; harus di-terminate. Bandwidth di atas kuota $0,01 per GB.

**Peringkat 6-11 — bagus untuk kebutuhan tertentu.** Cloudzy paling murah
di antara yang CPU-nya Genoa, dengan refund 14 hari sebagai jaring
pengaman karena nested-nya baru diklaim di halaman pemasaran. GreenCloud
promo tahunan punya nilai tertinggi di tabel tapi tidak bisa refund dan
stoknya musiman. SSD Nodes dan Contabo VDS S hanya masuk akal kalau
replica dipakai bergantian, bukan serentak.

**Dari sapuan kedua, belum masuk peringkat** karena nested-nya baru bukti
pengguna, bukan pernyataan resmi: V.PS Osaka Edge Orange (nilai 0,18,
tetapi Osaka, bukan Singapura, dan refund 14 hari untuk membuktikannya)
dan DMIT Hong Kong (nilai 0,07). Keduanya kalah dari Onidel HF dan
OVHcloud VPS-3 di semua sumbu kecuali kuota trafik, jadi hanya berguna
kalau Onidel dan Advin habis dan Haswell OVHcloud terlalu lambat.

**Peringkat 12-16 — hanya untuk alasan khusus:** server benar-benar di
Jakarta, atau tagihan per jam untuk uji coba beberapa jam. Untuk dipakai
24/7 semuanya kalah telak. Ukuran 12-16 GB-nya bukan tidak ada — GCE
n4-highmem-2, AWS r7i.large, dan OCI 2 OCPU / 16 GB semuanya memuat 3
replica default atau 6 replica headless 1536 MB — tetapi $76-117 per
bulan untuk 2 vCPU, dibanding $12,32 (OVHcloud VPS-3) atau $38,70
(Onidel HF-4) untuk 6 core dan RAM yang sama. Spot GCE Jakarta
(n4-highmem-2 ~$34) dan Spot AWS m7i-flex.xlarge (~$25) mendekati harga
VPS, tetapi bisa dimatikan Google/Amazon kapan saja, jadi cocok untuk
uji coba, bukan replica yang harus hidup terus.

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
hanya di bare metal. Untuk enam replica headless (butuh 10-12 GB), tipe
termurahnya n4-highmem-2 di GCE (~$117/bulan) atau r7i.large di AWS
(~$117/bulan); rinciannya ada di tabel cloud besar pada bagian
[Menerjemahkan beban](#menerjemahkan-beban-yang-mau-dijalankan-ke-ukuran-vps).

Di kalangan VPS murah, belum ditemukan penyedia di Jakarta atau Malaysia
yang menyatakan nested virtualization secara eksplisit. Perwira Cloud
(Jakarta, Singapura) dan Shinjiru (Kuala Lumpur, Cyberjaya) menjual VPS
KVM tetapi tidak menyebut nested; Biznet Gio (NEO Lite / NEO Virtual
Compute) dan IDCloudHost juga tidak menyebutkannya di halaman produk.
Tanyakan langsung apakah VPS-nya mengekspos vmx/svm ke tamu. Provider
yang memakai panel SolusVM bisa mengaktifkan nested per VPS kalau
diminta, jadi menanyakan itu sering membuahkan hasil.

Sapuan 7 September 2026 memeriksa 19 penyedia lokal lagi. Hasilnya sama:
tidak ada yang menulis nested di halaman produk, KB, atau ToS. Alasannya
tergambar di satu-satunya diskusi hoster lokal yang ditemukan
(DiskusiWebHosting): sebagian hypervisor mereka mendukung nested, tetapi
"sengaja tidak diinfokan agar tidak sembarangan orang pakai", dan VM
klien yang meminta dipindah ke node yang mendukung. Jadi untuk penyedia
lokal, tiket dulu, minta bukti `grep -c vmx /proc/cpuinfo`, baru bayar.
Yang layak ditanya, urut dari yang risikonya paling kecil:

| Provider | Lokasi | CPU | Harga (Rp/bulan, ≈ US$) | Refund | Catatan |
|---|---|---|---|---|---|
| **LightNode** | Jakarta (id-jakarta-1) | Xeon Skylake/Haswell | VPS 2 vCPU / 4 GB / 50 GB $14,70; 4 / 8 GB $27,70; 8 / 16 GB $52,70. VDS (CPU dedicated) 2 / 4 GB $40,70; 4 / 8 GB $80,70; 8 / 16 GB $158,70 | Tidak ada, tetapi tagihan per jam | Satu-satunya dengan indikasi resmi (materi VDS) dan bukti YABS `Enabled` di lokasi lain. Deploy VPS satu jam (~$0,02) untuk cek vmx dulu |
| **Nevacloud** | Jakarta (jkt-2) | Nevalite: Xeon Platinum Skylake; NVMe: EPYC Genoa | Nevalite 8 GB 4c / 100 GB Rp457.600 ($28); 12 GB 4c / 150 GB Rp624.800 ($38); 16 GB 6c / 200 GB Rp853.600 ($52). NVMe Genoa 8 GB Rp598.400 ($37); 16 GB Rp1.126.400 ($69) | Pro-rata ke saldo, saldo tidak bisa dicairkan; tagihan per jam | Kebijakan melarang VPS jadi router/VPN/MikroTik CHR, hati-hati kalau replica dipakai NAT |
| **DewaVPS / Dewaweb** | Jakarta (NEX), Singapura | High Performance: EPYC Genoa 9654; General: Xeon Gold 5218 | Kalkulator per jam: 1 vCPU Rp30.000, 1 GB Rp30.000, NVMe ~Rp1.250/GB. Perkiraan shared 4 vCPU / 8 GB / 100 GB ≈ Rp485.000 ($30); 8 / 16 GB / 200 GB ≈ Rp970.000 ($60) | Garansi 90 hari **tidak berlaku untuk cloud server**; tagihan per jam | CPU terbaik di daftar lokal |
| **Jagoan Hosting** | Indonesia (tidak spesifik) | EPYC / Xeon | Galaxy 4c / 4 GB / 100 GB Rp200.000 ($12); Universe 6c / 6 GB / 100 GB Rp300.000 ($18); Multiverse custom sampai 24c / 128 GB | **30 hari uang kembali**, gugur kalau trafik >10 GB atau pernah refund | Uji kvm-ok lalu refund kalau gagal; ToS melarang game server dan streaming |
| Herza Cloud | Jakarta, Depok, Surabaya, Cibitung (DCI Tier 4); juga SG, KL, HK, Manila | **Xeon E5-2697 v2 (2013)**; Tier 4: E5-2690 v4 | 4 vCPU / 8 GB / 100 GB Rp247.500 ($15); 8 / 12 GB / 150 GB Rp450.000 ($28); 8 / 16 GB / 200 GB Rp637.500 ($39); trafik unlimited | Halaman menyebut 30 hari, syarat tidak ditemukan | Termurah per GB RAM, tetapi single-core Ivy Bridge terlalu lemah untuk desktop di dalam replica |
| CloudKilat | Jakarta (NEX) | Xeon Gold 6230 | S 4c / 4 GB / 70 GB Rp360.000 ($22); M 8c / 8 GB / 120 GB Rp720.000 ($44); L 16c / 16 GB / 200 GB Rp1.440.000 ($88) | tidak ditemukan | Mahal untuk CPU 2019 |
| Rumahweb | Bogor, DCI Bekasi | tidak disebut | XL 4c / 8 GB / 160 GB Rp500.000 ($31, promo Rp250.000); X2L 8c / 16 GB / 300 GB Rp937.500 ($58) | **Tidak ada** ("VPS yang sudah aktif tidak dapat dibatalkan") | Disk besar, tetapi tanpa refund dan CPU tidak diketahui |
| Exabytes Indonesia / Malaysia | Indonesia (tidak spesifik); Malaysia Tier-3 | ID: kontradiktif ("AMD 64 core" dan "Xeon Hexa-Core"); MY: EPYC | ID C2 4c / 8 GB / 200 GB Rp493.000 ($30); M3 4c / 16 GB Rp659.000 ($40). MY C4 4c / 8 GB / 200 GB RM114 ($27); C6 8c / 16 GB / 400 GB RM222 ($53) | **Tidak ada** untuk VPS di keduanya ("biaya terminasi 100%") | Jangan tanpa konfirmasi tertulis |
| IP ServerOne (Malaysia) | Kuala Lumpur, juga SG, HK | tidak disebut | C4 4c / 15 GB RM161 ($38), disk hanya 10 GiB, SSD tambahan RM0,60/GiB | **Tidak ada** ("non-refund policy"); tagihan per jam | — |
| Casbay (Malaysia) | Cyberjaya | "Intel" tanpa model | VPS 8 8c / 8 GB / 160 GB $54,59-102,59 tergantung durasi | VPS dikecualikan; 30 hari hanya kalau masalah teknis dari Casbay, potongan $15 | — |
| Cloudmatika | Jakarta (NTT) | tidak disebut | Linux 8G Rp429.000 ($26) | trial 7 hari | **Lini Linux adalah container Virtuozzo**, bukan VM; tidak bisa KVM sama sekali. Hanya lini Windows yang VM |
| Datacomm DCloud | Jabodetabek | tidak disebut | GP4C8G 4c / 8 GB / 50 GB Rp920.000 ($56); GP8C16G Rp1.840.000 ($113) | tidak ditemukan | Mahal, disk kecil |
| Wowrack, Indonesian Cloud, Lintasarta Cloudeka, Telkomsigma Flou | Jakarta/Surabaya | — | Harga lewat sales atau kalkulator; hypervisor tidak disebut | — | Enterprise, bukan beli-coba |
| Tencent Cloud CVM, Huawei Cloud ECS | Jakarta | — | — | — | **Resmi melarang**: Tencent "Virtualized software cannot be installed or re-virtualized"; Huawei "Do not install virtualization software on ECSs for nested virtualization" |

Alternatif yang sering sudah cukup: GreenCloudVPS Singapura DC2 terukur
13,68 ms dari Jakarta pada review di atas, praktis sama dengan server
lokal untuk layar replica lewat browser.

## Saran

- Urutan lengkap 16 plan ada di [Peringkat akhir](#peringkat-akhir) di
  atas. Tiga teratas: Advin Servers EPYC Genoa $20, Advin $6 untuk satu
  replica, lalu OVHcloud VPS-3 sebagai pilihan yang stoknya selalu ada.
- RAM tidak bisa dijual berlebih: untuk enam replica headless ambil
  minimal 12 GB (6 GB dan 8 GB terbukti tidak cukup), untuk enam replica
  XFCE 16 GB. Di cloud besar ukuran itu ada (GCE n4-highmem-2, AWS
  r7i.large, OCI 2 OCPU / 16 GB) tetapi $76-117/bulan.
- Sapuan brand lain (7 September 2026, 60-an provider) tidak menemukan
  pesaing baru yang nested-nya resmi dan berlokasi di Asia. Cadangan
  kalau Onidel dan Advin habis: V.PS Osaka Edge (EPYC 7763, bukti YABS,
  refund 14 hari) dan DMIT Hong Kong (AUP hanya melarang komersial,
  refund 3 hari). Untuk Jakarta, uji LightNode per jam dulu.
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
  [KB: nested virtualization](https://kb.onidel.com/hc/kb/articles/1770746220-nested-virtualization),
  [Terms of Service (refund 14 hari, plan promo jadi kredit, top-up tidak refundable)](https://kb.onidel.com/hc/kb/articles/1756033443-terms-of-service),
  [cara kerja tagihan per jam](https://kb.onidel.com/hc/kb/articles/1756043421-how-does-hourly-billing-work),
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
  [tipe mesin dan harga di asia-southeast2](https://gcloud-compute.com/asia-southeast2.html),
  harga per tipe termasuk Spot: [n4-highmem-2](https://gcloud-compute.com/n4-highmem-2.html),
  [n4-standard-4](https://gcloud-compute.com/n4-standard-4.html),
  [n2-highmem-2](https://gcloud-compute.com/n2-highmem-2.html)
- AWS: [nested virtualization di EC2](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/amazon-ec2-nested-virtualization.html),
  [region Jakarta](https://aws.amazon.com/blogs/aws/now-open-aws-asia-pacific-jakarta-region),
  harga per region (termasuk ap-southeast-3): [m7i.large](https://www.devzero.io/instances/aws/m7i.large),
  [r7i.large](https://www.devzero.io/instances/aws/r7i.large),
  [m7i-flex.xlarge](https://www.devzero.io/instances/aws/m7i-flex.xlarge),
  [m7i.xlarge](https://www.devzero.io/instances/aws/m7i.xlarge)
- Oracle Cloud: [KVM nested virtualization di OCI](https://blogs.oracle.com/linux/kvm-nested-virtualization-in-oci),
  [daftar harga (Standard3 $0.04/OCPU-jam, $0.0015/GB-jam)](https://www.oracle.com/cloud/price-list/)
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
- Sapuan kedua, internasional: V.PS [plan per kota](https://v.ps/vps/),
  [Singapura](https://v.ps/locations/singapore/),
  [Edge KVM](https://v.ps/products/edge-kvm-vps/),
  [YABS Tokyo Performance dengan VM-x Enabled](https://vps.dance/xtom-tyo-perf.html);
  DMIT [harga](https://www.dmit.io/pages/pricing), [AUP](https://www.dmit.io/pages/aup),
  [ToS refund](https://www.dmit.io/pages/tos),
  [review VirtReady Yes](https://www.daniao.org/11004.html);
  UpCloud [harga](https://upcloud.com/pricing/),
  [YABS Singapura VM-x Disabled](https://www.letshosting.com/13669.html),
  [komentar LowEndTalk soal nested via support](https://lowendtalk.com/discussion/141719/list-of-vps-providers-with-nested-kvm-and-hourly-pricing);
  LightNode [harga](https://go.lightnode.com/), [ToS](https://www.lightnode.com/en-US/termsService),
  [YABS Dubai Enabled](https://www.vpsbenchmarks.com/yabs/lightnode_com-1c-2gb-906e76),
  [VDS vs VPS yang menyebut nested](https://go.lightnode.com/tech/vds-vs-vps);
  MassiveGRID [VDS](https://massivegrid.com/vds/), [Windows VDS soal nested](https://massivegrid.com/windows-vds/),
  [review LowEndTalk](https://lowendtalk.com/discussion/198672/a-quick-review-benchmark-of-massivegrid-promo-vps);
  Virtono [Cloud VPS](https://www.virtono.com/cloud-vps/), [ToS](https://www.virtono.com/terms-of-service.php),
  [YABS Tokyo](https://www.letshosting.com/12896.html);
  LayerStack [harga](https://www.layerstack.com/en/general-cloud-server), [ToS](https://www.layerstack.com/docs/legal/TOS.php);
  HostUS [KVM](https://hostus.us/kvm-vps.php), [ToS](https://hostus.us/terms-and-conditions.php);
  Regxa [KVM](https://regxa.com/kvm-vps), [refund](https://regxa.com/legal/refund-policy);
  Vodien [Linux VPS](https://www.vodien.com/linux-vps-hosting/);
  ZgoVPS [ToS](https://zgovps.com/terms-of-service/), [hardware](https://zgovps.com/data-center-hardware/);
  Netcup [FAQ SVM disabled](https://netcup.com/en/helpcenter/faq),
  [Singapura dibuka](https://lowendtalk.com/discussion/212942/netcup-launches-singapore-location);
  Leaseweb [YABS Singapura Disabled 2026](https://www.vpsbenchmarks.com/yabs/leaseweb-6c-16gb-20260831-d59511),
  [thread Singapura](https://lowendtalk.com/discussion/202576/leaseweb-singapore-vps-amd-epyc-25tb-bandwidth-10gbit);
  BandwagonHost [ToS](https://bandwagonhost.com/terms-of-service.php);
  RackNerd [ToS](https://www.racknerd.com/terms-of-service);
  Hostwinds [tutorial virtualisasi](https://www.hostwinds.com/tutorials/virtualization-on-hostwinds-servers-dedicated);
  Webdock [FAQ nested](https://docs.webdock.io/faq/is-nested-virtualization-supported);
  Katapult [YABS London](https://www.letshosting.com/12254.html), [harga](https://krystalhosting.com/cloud/pricing);
  Terabit [YABS AMD-V Enabled](https://lowendtalk.com/discussion/205553/transfer-terabit-silvercreek-8-8-88-vps);
  Servarica [plan KVM Slim yang menyebut nested](https://servarica.com/plan/613/);
  diskusi [seberapa umum nested di VPS](https://lowendtalk.com/discussion/207763/how-common-is-nested-virtualization-in-vps-plans-should-i-expect-proxmox-ve-to-work-on-a-vps)
- Sapuan kedua, Indonesia dan Malaysia:
  [DiskusiWebHosting: nested sengaja tidak diumumkan](https://www.diskusiwebhosting.com/threads/nested-virtualization-di-hypervisor.24552/);
  LightNode [Jakarta VPS](https://go.lightnode.com/indonesia-vps), [Jakarta VDS](https://go.lightnode.com/jakarta-vds);
  Nevacloud [harga](https://nevacloud.com/harga/), [kebijakan](https://nevacloud.com/kebijakan-layanan/);
  DewaVPS [harga](https://www.dewavps.com/cloud-vps-server-murah-indonesia/), [SLA](https://www.dewaweb.com/service-level-agreement);
  Jagoan Hosting [VPS](https://www.jagoanhosting.com/vps-indonesia/), [aturan layanan](https://www.jagoanhosting.com/aturan-layanan/);
  Herza [VPS murah](https://herza.id/vps-murah/);
  CloudKilat [Kilat VM](https://vm.cloudkilat.com/);
  Rumahweb [VPS](https://www.rumahweb.com/vps-murah/);
  Exabytes [Indonesia](https://www.exabytes.co.id/server/vps-linux-ssd), [Malaysia](https://www.exabytes.my/servers/nvme-vps),
  [money-back Malaysia](https://www.exabytes.my/money-back-guarantee);
  IP ServerOne [harga](https://www.ipserverone.com/pricing/), [syarat](https://www.ipserverone.com/terms-and-conditions/);
  Casbay [plan](https://www.casbay.com/vps-hosting-malaysia/plans), [ToS](https://www.casbay.com.my/terms);
  Cloudmatika [store](https://cloudmatika.com/store/index.php?NAME_PATH=VPS_LINUX);
  Datacomm [harga](http://dcloud.co.id/en/pricing.html);
  Tencent Cloud [batasan CVM](https://www.tencentcloud.com/document/product/213/15379);
  Huawei Cloud [batasan ECS](https://support.huaweicloud.com/intl/en-us/productdesc-ecs/ecs_01_0004.html)
