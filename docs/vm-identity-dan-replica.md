# Identitas VM dan Replica di Nestbox: penjelasan untuk orang awam

Dokumen ini menjelaskan, tanpa istilah teknis yang berat, apa yang terjadi
saat kamu mengisi **VM identity** di Nestbox dan apa itu **replica**
(kolom 🪆 di daftar sandbox). Kalau mau detail teknisnya, lihat
`tools/linux/identity/README.md`, `tools/linux/replica/README.md`, dan
`tools/linux/replica/qemu-identity/README.md`.

## Masalahnya: sebuah VM selalu "mengaku" sebagai VM

Setiap komputer punya semacam KTP yang bisa dibaca oleh program di dalamnya:
merek dan tipe motherboard, versi BIOS, nomor seri, merek prosesor, nama
harddisk dan DVD, dan sebagainya. Sistem operasi dan aplikasi (termasuk
Steam, anti-cheat, dan alat lisensi) membaca KTP ini.

Di mesin virtual, KTP itu diisi oleh hypervisor (di Windows: Hyper-V) dan
isinya jujur: "Microsoft Corporation, Virtual Machine". Selain KTP, ada dua
"tanda lahir" lain yang lebih dalam:

- **Bit hypervisor di CPU**: prosesor sendiri memberi tahu "kamu sedang di
  dalam VM".
- **Tanda tangan hypervisor** (nama teknisnya CPUID 0x40000000): kalau
  ditanya "siapa yang menjalankanmu?", CPU menjawab "Microsoft Hv" atau
  "KVMKVMKVM".

Perintah `systemd-detect-virt` di Linux membaca semua itu dan menjawab
`microsoft`, `kvm`, atau `none` (bare metal).

## Dua lapisan yang dipakai Nestbox

Ada dua tempat berbeda di mana identitas bisa diubah, dan keduanya punya
batas yang berbeda.

### Lapisan 1: sandbox VM (tempat Steam-mu terinstall)

Sandbox VM adalah VM Ubuntu yang berjalan di Hyper-V. Di sini Nestbox
**tidak bisa mengganti KTP aslinya**, tapi bisa menaruh "stiker" di atasnya:

- Setiap program yang membaca KTP lewat jalur normal Linux
  (`/sys/class/dmi/id`, `dmidecode`, `hostnamectl`, `systemd-detect-virt`)
  mendapat nilai dari profil, misalnya "ASUS / ROG STRIX Z690-F".
- `systemd-detect-virt` menjawab `none` seperti di komputer asli.

Yang **tidak bisa** ditutupi stiker: bit hypervisor dan tanda tangan
hypervisor, karena itu datang langsung dari CPU/Hyper-V. Program yang
sengaja bertanya ke CPU tetap tahu ini VM. Perintah `lscpu` misalnya tetap
menampilkan "Hypervisor vendor: Microsoft".

### Lapisan 2: replica (VM di dalam VM)

Replica adalah VM kedua yang berjalan **di dalam** sandbox VM, memakai
QEMU/KVM (bukan Hyper-V). Karena QEMU-nya kita bangun sendiri dengan
patch, semua tanda lahir tadi bisa diatur, bukan sekadar ditempeli
stiker:

| Yang dibaca program | Sandbox VM (stiker) | Replica (asli diubah) |
|---|---|---|
| Merek/tipe motherboard, BIOS, nomor seri | ✅ | ✅ |
| `systemd-detect-virt` | ✅ `none` | ✅ `none` (bahkan sebagai root) |
| `hostnamectl` (Chassis, Hardware Vendor) | ✅ | ✅ |
| Merek prosesor dan RAM di SMBIOS | ❌ | ✅ |
| Nama DVD/harddisk (contoh: HL-DT-ST) | ❌ | ✅ |
| Tabel ACPI (ALASKA / A M I, bukan BOCHS) | ❌ | ✅ |
| Bit hypervisor di CPU | ❌ masih ada | ✅ disembunyikan |
| Tanda tangan hypervisor | ❌ Microsoft Hv | ✅ bebas (default `GenuineIntel`) |
| Flag "Virtual Machine" di SMBIOS | ❌ | ✅ dimatikan |
| Daftar PCI (`lspci`) | ❌ Hyper-V | ❌ masih ada virtio/Cirrus |

Harga yang dibayar: replica **tidak punya GPU**. Grafisnya digambar oleh CPU
(llvmpipe). Cukup untuk desktop, Steam client, dan game ringan; tidak untuk
game berat.

Analogi: sandbox VM itu rumah kontrakan yang kamu cat ulang; tetangga yang
melihat dari luar masih tahu itu kontrakan. Replica itu rumah kecil yang kamu
bangun sendiri di halamannya, dari nol, sesuai selera.

## Hasil yang sudah terbukti

Dengan profil default (ASUS/AMI), inilah yang dilaporkan **dari dalam
replica** oleh `appsandbox-replica checks`:

```
sys_vendor             ASUS
product_name           System Product Name
bios_vendor            American Megatrends Inc. 2604
CPUID 0x40000000       GenuineIntel
hypervisor CPU flag    not set
systemd-detect-virt    none
acpi_oem_id            ALASKA
smbios_manufacturer    Intel(R) Corporation
smbios_vm_bit          not set
cdrom_model            DVDRAM GH24NSD1
```

Dengan kata lain, dari sudut pandang program di dalamnya, replica bukan VM:
tidak ada bit hypervisor, tidak ada tanda tangan hypervisor yang dikenal,
tidak ada flag "Virtual Machine", dan semua KTP menyebut perangkat asli.
Yang tersisa hanya daftar PCI (lihat FAQ di bawah).

## Dari mana patch QEMU-nya

QEMU asli menulis nama-namanya sendiri ke dalam KTP mesin: "QEMU HARDDISK",
"BOCHS", "KVMKVMKVM". Proyek [kila58/qemu-patched](https://github.com/kila58/qemu-patched)
mengganti semua itu dengan kata "TOSH" yang di-hardcode. Kami mengambil ide
yang sama tapi membuatnya bisa diatur: setiap nama membaca variabel
lingkungan `QEMU_IDENTITY_*`, dan Nestbox mengisinya dari profil VM
identity-mu. Satu build QEMU cukup untuk profil apa pun, dan kalau tidak
diisi, QEMU berperilaku persis seperti aslinya. Ada tujuh patch kecil di
`tools/linux/replica/qemu-identity/patches/`.

QEMU-nya dibangun di dalam sandbox VM dari kode sumber resmi (QEMU 8.2.2,
sama dengan versi Ubuntu 24.04), lalu dipasang menggantikan QEMU bawaan
Ubuntu dengan cara yang bisa dibatalkan (`qemu restore`).

## Apa yang terjadi saat kamu menekan Save di editor 🪪

1. Profil (JSON) disimpan di host, di `C:\ProgramData\AppSandbox\vms.cfg`.
2. Host mengirimnya ke agent di dalam sandbox VM.
3. Agent menyimpannya di `/etc/appsandbox/identity.json` dan menjalankan
   `appsandbox-identity apply`: memasang stiker DMI, membelokkan
   `dmidecode` / `systemd-detect-virt`, menyetel chassis di `hostnamectl`.
   Ini juga diulang otomatis setiap sandbox VM boot.
4. Kalau ada replica, agent menjalankan `appsandbox-replica reidentify`:
   definisi replica ditulis ulang dari profil yang sama. Karena KTP dibaca
   sekali saat boot, replica baru memakai identitas baru **setelah
   di-restart** (tombol "Restart replica" di jendela layarnya).

Profil default yang terisi otomatis di New Sandbox meniru PC rakitan ASUS
dengan BIOS AMI. Nilai seperti "System Product Name" dan "Default string"
memang persis yang dilaporkan board ASUS asli, jadi tidak terlihat
dibuat-buat.

## Cara pakai dari GUI

Kolom di daftar sandbox, dari kiri ke kanan setelah Snapshot:

| Ikon | Fungsi |
|---|---|
| ▶️ | Start/stop sandbox VM |
| 📺 | Layar **utama** sandbox VM (GPU penuh) |
| `>_` | Terminal SSH ke sandbox VM |
| 🪆 | **Replica**: ▶ untuk menyalakan; ⏳ saat sedang boot; 🖥️ membuka layarnya di dalam Nestbox |
| 🪪 | Editor VM identity (profil JSON) |

Jendela layar replica punya tombol Ctrl+Alt+Del, Fit/1:1, External viewer
(kalau lebih suka TigerVNC), Restart replica, dan Stop replica. Replica ikut
hidup otomatis setiap sandbox VM boot.

## Menyiapkan replica pertama kali (sekali saja per sandbox VM)

Masuk lewat tombol `>_`, lalu:

```
sudo appsandbox-replica install       # pasang QEMU + libvirt (sekali)
sudo appsandbox-replica qemu build    # bangun QEMU yang dipatch (~10 menit)
sudo appsandbox-replica create        # unduh Ubuntu, buat replica
sudo appsandbox-replica desktop       # XFCE + Steam + autologin (10-20 menit)
```

Setelah itu semuanya dari GUI. Perintah lain yang berguna:

```
sudo appsandbox-replica checks               # cetak semua "KTP" yang dilihat dari dalam replica
sudo appsandbox-replica reidentify --restart # terapkan profil baru sekarang
sudo appsandbox-replica ssh                  # terminal ke dalam replica (user / test123)
sudo appsandbox-replica qemu restore         # kembalikan QEMU bawaan Ubuntu
sudo appsandbox-identity status              # apa yang dilihat sandbox VM saat ini
```

## Pertanyaan yang sering muncul

**Kenapa Steam di sandbox VM tidak dipindah saja ke replica?**
Bisa (perintah `desktop` memasangnya), tapi replica tidak punya GPU. Kalau
game-nya butuh GPU, tetap jalankan di sandbox VM; di sana yang berubah hanya
lapisan stiker.

**Bisa tidak QEMU yang dipatch dipakai langsung untuk sandbox VM?**
Tidak. Sandbox VM dijalankan oleh Hyper-V, bukan QEMU, dan Hyper-V tidak
menyediakan cara menyembunyikan bit hypervisor. Kalau mau semuanya asli
sekaligus GPU penuh, jalannya adalah host Linux dengan KVM dan GPU
passthrough, di luar Nestbox.

**Kenapa nilai ACPI-ku terpotong?**
Kolom ACPI punya lebar tetap: OEM ID 6 huruf, table ID 8 huruf, creator ID 4
huruf. "HETZNER" menjadi "HETZNE".

**Kenapa clocksource di replica jadi `acpi_pm`, bukan `kvm-clock`?**
Karena tanda tangan hypervisornya bukan lagi KVM, kernel tidak memakai jam
KVM. Itu memang efek yang diinginkan kalau ingin terlihat bare metal.

**Apa yang masih membocorkan bahwa ini VM?**
Di sandbox VM: bit hypervisor, tanda tangan hypervisor, daftar PCI, dan
baris "Virtualization: microsoft" di `hostnamectl`. Di replica: daftar PCI
(virtio, Cirrus/virtio VGA) dan ID USB tablet `0627:0001` milik QEMU.

## Kalau ada yang tidak jalan

- Log Nestbox (panel bawah) menampilkan baris seperti
  `[myappsandbox] VM identity: applied: ...` atau `VM identity FAILED: ...`.
- `[myappsandbox] Nested replica: running` berarti host tahu replica hidup;
  kalau kolom 🪆 tetap kosong, tutup dan buka lagi Nestbox.
- Di dalam sandbox VM, `sudo appsandbox-identity status` dan
  `sudo appsandbox-replica status` menunjukkan keadaan saat ini.
