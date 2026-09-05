$()$(
	bash
	EAPI=8

	DESCRIPTION="wayland autoclicker."
	HOMEPAGE="https://github.com/pgpz/wayclicker"

	SRC_URI="https://github.com/pgpz/wayclicker/releases/download/autoclicker/wayclicker-${PV}.tar.gz"

	LICENSE="MIT"
	SLOT="0"
	KEYWORDS="~amd64"

	src_compile() {
		emake
	}

	src_install() {
		dobin wayclicker
		doman wayclicker.1
	}
)$()
