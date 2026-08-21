# Panduan Lengkap: Dari Nol Sampai VoiceMorph.exe

Panduan ini mengasumsikan Anda belum pernah memakai GitHub sama sekali.
Tidak ada perintah terminal yang perlu diketik. Semua lewat klik.

**Yang Anda butuhkan:** komputer Windows, koneksi internet, sekitar 30 menit
(20 menit di antaranya hanya menunggu).

**Yang akan Anda dapat:** file `VoiceMorph-0.1.0-Windows-Setup.exe` yang bisa
di-install seperti software biasa.

---

## Kenapa lewat GitHub?

Kode program harus di-*compile* dulu menjadi `.exe`. Untuk itu biasanya perlu
memasang Visual Studio — sekitar 10 GB dan sering bermasalah bagi pemula.

GitHub punya layanan gratis bernama **Actions**: mereka meminjamkan komputer
Windows di internet, meng-compile kode Anda di sana, lalu memberi Anda file
`.exe` yang sudah jadi. Anda hanya perlu mengunggah kodenya.

Jadi alurnya: **unggah kode → tunggu → unduh .exe**

---

# BAGIAN 1 — Persiapan

## Langkah 1: Buat akun GitHub

1. Buka **https://github.com/signup**
2. Isi email, buat password, pilih username (bebas, misalnya `budisantoso`)
3. Verifikasi lewat email yang dikirim ke Anda
4. Kalau ditanya paket, pilih **Free**

Selesai. Akun gratis sudah cukup untuk semua yang kita butuhkan.

## Langkah 2: Pasang GitHub Desktop

Ini aplikasi resmi GitHub yang membuat kita tidak perlu mengetik perintah
apa pun.

1. Buka **https://desktop.github.com**
2. Klik **Download for Windows**
3. Jalankan file yang terunduh, tunggu sampai selesai
4. Aplikasi akan terbuka sendiri dan meminta login — klik
   **Sign in to GitHub.com**, lalu masuk dengan akun tadi
5. Kalau diminta "Configure Git", klik **Finish** saja

## Langkah 3: Ekstrak file proyek

1. Cari file **`VoiceMorph-repo.zip`** yang sudah Anda unduh
2. Klik kanan → **Extract All...** → **Extract**
3. Anda akan mendapat folder bernama **`VoiceMorph`**
4. **Ingat baik-baik di mana folder ini berada.** Misalnya
   `C:\Users\Budi\Downloads\VoiceMorph`

> **Penting:** jangan mengubah nama folder atau memindahkan isinya. Struktur
> foldernya sudah benar apa adanya.

---

# BAGIAN 2 — Unggah kode ke GitHub

## Langkah 4: Tambahkan folder ke GitHub Desktop

1. Di GitHub Desktop, klik menu **File** → **Add local repository...**
2. Klik **Choose...** dan arahkan ke folder `VoiceMorph` hasil ekstrak tadi
3. Klik **Select Folder**, lalu klik **Add repository**

Kalau muncul pesan "this directory does not appear to be a Git repository",
berarti Anda salah memilih folder — pastikan Anda memilih folder `VoiceMorph`
itu sendiri, bukan folder di atasnya dan bukan folder di dalamnya.

Kalau berhasil, GitHub Desktop akan menampilkan nama **VoiceMorph** di pojok
kiri atas.

## Langkah 5: Publikasikan

1. Klik tombol biru besar **Publish repository** di bagian atas
2. Sebuah jendela muncul:
   - **Name:** biarkan `VoiceMorph`
   - **Keep this code private:** boleh dicentang atau tidak, keduanya bekerja
3. Klik **Publish repository**
4. Tunggu beberapa detik sampai proses unggah selesai

Kode Anda sekarang sudah ada di GitHub, dan **proses compile otomatis sudah
mulai berjalan saat itu juga.**

---

# BAGIAN 3 — Tunggu proses compile

## Langkah 6: Lihat prosesnya

1. Di GitHub Desktop, klik menu **Repository** → **View on GitHub**
   (browser akan terbuka ke halaman repo Anda)
2. Di bagian atas halaman, klik tab **Actions**
3. Anda akan melihat satu baris dengan judul mirip
   *"VoiceMorph: source/filter voice transformer..."*

Arti simbol di sebelahnya:

| Simbol | Artinya |
|---|---|
| Lingkaran kuning berputar | Sedang di-compile, tunggu |
| Centang hijau | Berhasil, lanjut ke Langkah 7 |
| Silang merah | Gagal — lihat bagian "Kalau gagal" di bawah |

**Berapa lama?** Sekitar 8–15 menit untuk yang pertama. Anda boleh menutup
browser dan kembali lagi nanti; prosesnya jalan di server GitHub, bukan di
komputer Anda.

---

# BAGIAN 4 — Unduh hasilnya

## Langkah 7: Ambil installer

1. Setelah muncul centang hijau, **klik judul baris tersebut**
2. Scroll ke bagian paling bawah halaman, cari kotak berjudul **Artifacts**
3. Klik **VoiceMorph-Windows-Setup** — sebuah file `.zip` akan terunduh
4. Ekstrak zip itu (klik kanan → Extract All)
5. Di dalamnya ada **`VoiceMorph-0.1.0-Windows-Setup.exe`**

Itulah installer Anda.

## Langkah 8: Install

1. Klik dua kali file `.exe` tersebut
2. **Windows akan menampilkan layar biru bertuliskan "Windows protected your
   PC".** Ini normal dan bukan virus — muncul karena installer tidak
   ditandatangani secara digital, yang memerlukan sertifikat berbayar sekitar
   4 juta rupiah per tahun.
   - Klik **More info**
   - Klik **Run anyway**
3. Ikuti wizard installer, klik Next sampai selesai
4. Di akhir akan muncul pesan tentang VB-Cable — baca, lalu klik OK

---

# BAGIAN 5 — Memakai VoiceMorph

## Langkah 9: Coba dulu tanpa apa-apa

1. Buka **VoiceMorph** dari Start Menu
2. Klik ikon **gerigi/setting** di pojok
3. Atur:
   - **Input:** mikrofon Anda
   - **Output:** speaker atau headphone Anda
4. **Pakai headphone**, jangan speaker, kalau tidak suara akan melengking
   (feedback)
5. Bicaralah. Putar knob **PITCH** dan **FORMANT**

Kalau kurva biru di layar bergerak saat Anda bicara, semuanya bekerja.

**Coba ini dulu:** geser slider **GENDER** pelan-pelan ke kanan. Itu cara
tercepat merasakan apa yang dilakukan software ini.

## Langkah 10: Supaya bisa dipakai di Discord / OBS / game

VoiceMorph tidak bisa langsung menjadi mikrofon. Perlu satu program perantara
yang gratis: **kabel audio virtual**.

1. Buka **https://vb-audio.com/Cable/**
2. Klik **Download** → ekstrak zip-nya
3. Klik kanan **`VBCABLE_Setup_x64.exe`** → **Run as administrator**
4. Klik **Install Driver**, lalu **restart komputer**

Setelah restart:

5. Buka VoiceMorph → setting → ubah **Output** menjadi **CABLE Input**
6. Di Discord: Settings → Voice & Video → **Input Device** pilih
   **CABLE Output**

Sekarang teman-teman di Discord mendengar suara Anda yang sudah diubah.

> Karena output VoiceMorph sekarang ke kabel virtual, Anda sendiri tidak
> mendengarnya lagi. Itu normal.

---

# Kalau gagal

## Build merah (silang) di tab Actions

Ini kemungkinan besar terjadi, dan bukan salah Anda — kode ini belum pernah
melewati compiler, jadi wajar kalau ada satu-dua kesalahan kecil.

Cara melaporkannya ke saya:

1. Klik baris yang merah
2. Klik kotak bertuliskan **windows** di sebelah kiri
3. Cari baris yang berwarna merah atau diawali `error C` / `error:`
4. **Salin 10–20 baris di sekitar error pertama** dan kirimkan ke saya

Error pertama yang penting. Error-error setelahnya biasanya hanya akibat
beruntun dari yang pertama.

## Tab Actions kosong / tidak ada apa-apa

Kemungkinan file `.github` tidak ikut terunggah. Cek di halaman repo GitHub
Anda: harus ada folder bernama `.github`. Kalau tidak ada, ulangi Langkah 4–5
dan pastikan Anda memilih folder `VoiceMorph` yang benar.

## GitHub Desktop bilang "repository not found" saat Publish

Anda belum login. Klik **File** → **Options** → **Accounts** → **Sign in**.

## Suara pecah, patah-patah, atau berderak

Di setting audio VoiceMorph, naikkan **Buffer Size** ke 512 atau 1024.
Latensi bertambah sedikit, tapi suara jadi bersih.

## Suara terdengar logam / berdengung

Naikkan knob **GATE** sedikit demi sedikit sampai suara desis ruangan hilang
ketika Anda diam. Ini penyebab paling umum.

---

# Yang belum berfungsi

Bagian **NEURAL CONVERSION** di bawah antarmuka belum bisa dipakai. Itu bagian
yang meniru Vocoflex — mengubah suara Anda menjadi *orang lain*, bukan sekadar
lebih tinggi atau lebih besar.

Bagian itu memerlukan file model AI (`.onnx`) yang harus dilatih terpisah, dan
masih ada satu komponen yang belum saya tulis (pelacak nada RMVPE). Penjelasan
lengkapnya ada di `README.md`.

**Yang sudah berfungsi penuh** adalah PITCH, FORMANT, dan GENDER — dan untuk
kebutuhan pria↔wanita, itu justru bagian yang paling penting.
