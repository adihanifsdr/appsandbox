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
| ExtraVM | Ya, aktif default di semua VPS | sekitar $4.50/bulan | Singapura (Equinix SG3), Tokyo, Sydney | 5 hari money-back untuk VPS, potongan 4% untuk refund di atas $25, pembayaran kripto tidak bisa refund |
| GreenCloud VPS | Ya. Review pihak ketiga dari Indonesia di plan Budget KVM Singapura DC2 menunjukkan "VM-x/AMD-V: Enabled" dan memakai nested | Budget/SSD KVM dari $6/bulan (1 core, 1 GB RAM, 15 GB SSD — terlalu kecil untuk replica, ambil minimal SSDKVM-3 $20/bulan dengan 30 GB); plan promo tahunan pernah $25/tahun untuk 2 core EPYC, 4 GB RAM, 35 GB NVMe | Singapura DC1 & DC2, Tokyo, Hong Kong, Hanoi, Ho Chi Minh | 7 hari untuk VPS pertama di akun baru; plan diskon/promo dan pembayaran kripto tidak termasuk |
| SSD Nodes | Ya, aktif default di semua plan | sekitar $14.50/bulan untuk 32 GB RAM, tapi kontrak 3 tahun dibayar di muka | Singapura, Tokyo, Mumbai, Sydney | 14 hari full refund lewat tiket support; setelah itu hanya kredit akun |
| DigitalOcean Droplet | Ya di semua region; DigitalOcean sendiri tidak merekomendasikan karena performa nested sering buruk | $6/bulan, ditagih per jam | Singapura (SGP1) | Tidak ada refund, tapi tagihan per jam sehingga uji beberapa jam hanya berbiaya sen |
| Contabo Cloud VDS | Ya. VDS adalah VM dengan core dan RAM dedicated, bukan bare metal | VDS S sekitar €49.40/bulan, VDS M €64.40, VDS L €91.60, sudah termasuk location fee Singapura | Singapura, Jepang, India, Australia | 14 hari money-back untuk akun pribadi, juga untuk perpanjangan otomatis dalam 72 jam terakhir; proses sampai 14 hari kerja |

Dua kandidat yang belum bisa dipastikan dari sumber primer: **WebHorizon**
(Singapura, Ryzen 9000 dan EPYC, sekitar $3-5/bulan) berulang kali menulis
"nested virtualization enabled" di penawaran resminya di forum, tetapi
situs mereka menolak diambil otomatis, jadi tanyakan dulu lewat tiket.
**HostHatch** (Singapura, Tokyo, Hong Kong) disebut pengguna mendukung
nested, tanpa pernyataan resmi di halaman produk.

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

| Plan | Harga/bulan | vCPU | RAM | Disk | Kuota traffic | Replica muat | Biaya per replica |
|---|---|---|---|---|---|---|---|
| OVHcloud VPS-1 | $4,54 | 2 | 4 GB | 40 GB NVMe | 500 GB | 1, tapi RAM replica harus diturunkan ke ~2,5 GB | $4,54 |
| OVHcloud VPS-2 | $8,50 | 4 | 8 GB | 75 GB NVMe | 1 TB | 1 penuh (2 kalau masing-masing 3 GB) | $8,50 |
| **OVHcloud VPS-3** | **$12,32** | 6 | 12 GB | 100 GB NVMe | 1 TB | **2** | **$6,16** |
| OVHcloud VPS-4 | $23,37 | 8 | 24 GB | 200 GB NVMe | 3 TB | 5 | $4,67 |
| GreenCloud SSDKVM-3 | $20 | 2 | 4 GB | 30 GB | 750 GB-6 TB (APAC) | 1 | $20 |
| GreenCloud SSDKVM-5 | $80 | 8 | 16 GB | 120 GB | idem | 3 | $26,7 |
| GreenCloud promo tahunan | ~$2,08 ($25/tahun) | 2 | 4 GB | 35 GB NVMe | 750 GB | 1 | $2,08 |
| ExtraVM 8 GB | $32 | 4 | 8 GB | 120 GB NVMe | 8 TB | 1-2 | $16-32 |
| ExtraVM 16 GB | $56 | 6 | 16 GB | 240 GB NVMe | 10 TB | 3 | $18,7 |
| SSD Nodes KVM/2X-LARGE | ~$11,08 ($133/tahun) | 8 Intel Silver | 32 GB | 480 GB SSD | besar | 7 | $1,58 |
| DigitalOcean 4 GB | $24 | 2 | 4 GB | 80 GB | 4 TB | 1 | $24 |
| DigitalOcean 8 GB | $48 | 4 | 8 GB | 160 GB | 5 TB | 1-2 | $24-48 |
| Contabo Cloud VDS S | €42,99 (€34,40 kontrak 12 bulan) | 3 core **fisik** EPYC | 24 GB | 180 GB NVMe | besar | 5 | sekitar €7-9 |
| GCE Jakarta n2-standard-2 | $76,28 + disk | 2 | 8 GB | dibeli terpisah (~$0,13/GB) | keluar Google dibayar per GB | 1-2 | $40-83 |
| AWS Jakarta m7i.large | sekitar $0,10-0,12/jam (≈$75-90 kalau 24/7) + EBS | 2 | 8 GB | EBS terpisah | egress dibayar per GB | 1-2 | $40-90 |
| OCI Singapura VM.Standard3.Flex 1 OCPU / 8 GB | sekitar $38 + block volume | 2 thread | 8 GB | terpisah | 10 TB gratis/bulan | 1 | ~$38 |

Tiga hal yang tidak kelihatan di tabel tapi menentukan rasa pakainya:

- **Umur CPU.** VPS OVHcloud yang dicek memakai Haswell (2013-an).
  Murah dan terbukti jalan, tetapi kecepatan satu core-nya jauh di bawah
  EPYC 4004/Ryzen 9 di ExtraVM atau EPYC di GreenCloud dan Contabo.
  Desktop XFCE dan Steam di dalam replica menggambar lewat CPU, jadi
  bedanya terasa. Nested sendiri sudah memotong sekitar 10% menurut
  Google, dan replica di dalam VPS berarti dua lapis virtualisasi.
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

### Worth it-nya

- **Paling masuk akal untuk dipakai sehari-hari: OVHcloud VPS-3, $12,32.**
  Speknya (6 vCPU, 12 GB, 100 GB) praktis sama dengan server yang sudah
  dicek dan terbukti menjalankan replica, muat 2 replica, kuota 1 TB, dan
  bisa dibatalkan 14 hari. $6,16 per replica adalah harga terbaik yang
  tidak menuntut bayar di muka bertahun-tahun.
- **Paling murah kalau berani prabayar: SSD Nodes**, $1,58 per replica
  per bulan untuk 7 replica sekaligus. CPU Intel Silver-nya lambat per
  core dan uangnya terkunci 1-3 tahun, jadi ini pilihan kalau yang
  dikejar jumlah replica, bukan kelincahan tiap replica.
- **Paling murah untuk satu replica coba-coba: GreenCloud promo tahunan**
  (~$2/bulan, sudah terbukti nested-nya di review dari Indonesia, 13,68 ms
  dari Jakarta) — asal stoknya ada dan sadar plan promo tidak bisa refund.
  Tanpa promo, GreenCloud $20 untuk 1 replica kalah telak dari OVH.
- **Paling enak dipakai (desktop + Steam): Contabo Cloud VDS S.** Core-nya
  fisik, bukan shared, jadi replica tidak ikut melambat saat tetangga
  sibuk; 24 GB RAM cukup untuk 5 replica di sekitar €7-9 per replica.
  Ini yang dipilih kalau replica dipakai serius, bukan sekadar dicoba.
- **Cloud besar hanya untuk dua alasan:** butuh server benar-benar di
  Jakarta, atau butuh hitungan per jam. GCE Jakarta n2-standard-2 dengan
  spek setara OVH VPS-2 harganya sekitar sembilan kali lipat kalau
  dinyalakan sebulan penuh. Untuk mengetes Nestbox setengah hari, itu
  sekitar $1,3 dan tidak ada komitmen — di situ dia menang.
- **Hindari untuk Nestbox:** semua plan dengan disk 15 GB (GreenCloud $6
  dan $10, ExtraVM $4,50). Base image 3,5 GB ditambah satu replica sudah
  melewatinya, jadi murahnya percuma.

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

- Ringkasan harga per replica dan pilihan terbaik ada di
  [Perbandingan spek](#perbandingan-spek-berapa-replica-yang-muat-dan-berapa-harganya)
  di atas: OVHcloud VPS-3 untuk pemakaian harian, Contabo VDS S kalau
  replica dipakai serius, cloud besar kalau butuh Jakarta atau tagihan
  per jam.
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
