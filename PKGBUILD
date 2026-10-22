# Checkout build: dotfiles pins fresh clones; local edits are included intentionally.
pkgname=vis-retained-terminal
pkgver=0.9.r501.g254329d9
pkgrel=3
pkgdesc='Vis with retained buffers and asynchronous terminal mode (including lexers)'
arch=('x86_64' 'aarch64')
url='https://github.com/JacksonDCollins/vis'
license=('ISC' 'MIT')
depends=('acl' 'bash' 'file' 'glibc' 'lua' 'lua-lpeg' 'ncurses' 'tre' 'libvterm'
         'libacl.so' 'libncursesw.so')
makedepends=('git')
optdepends=('wl-clipboard: Wayland clipboard support'
            'xclip: X11 clipboard support'
            'xsel: alternative X11 clipboard support')
provides=("vis=$pkgver" "vis-lexers=$pkgver")
conflicts=('vis' 'vis-lexers')
source=()
sha256sums=()

# Keep the checkout untouched and include local development changes.
prepare() (
    set -o pipefail
    rm -rf -- "${srcdir:?}/vis"
    mkdir -p "$srcdir/vis"
    cd "$startdir"
    git ls-files --cached --others --exclude-standard -z |
        bsdtar --null -T - -cf - |
        bsdtar -xf - -C "$srcdir/vis"
)

build() {
    cd "$srcdir/vis"
    ./configure --prefix=/usr --enable-curses --enable-lua --enable-tre \
        --enable-acl --disable-selinux
    make VERSION="v${pkgver}-retained-terminal" API=3234
}

package() {
    cd "$srcdir/vis"
    make DESTDIR="$pkgdir" VERSION="v${pkgver}-retained-terminal" API=3234 install
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
