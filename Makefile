# Created by: Thierry Dussuet <dussuett@wigwam.ethz.ch>
# $FreeBSD: head/sysutils/ods2/Makefile 410522 2016-03-07 14:59:02Z amdmi3 $

PORTNAME=	ods2
PORTVERSION=	1.3
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/FreeBSD-pzn/ods2/raw/master/
DISTNAME=	ods2

MAINTAINER=	ports@FreeBSD.org
COMMENT=	Utility for manipulating ODS-2 filesystems

#USES=		zip
NO_WRKSUBDIR=	yes
MAKEFILE=	makefile.unix
MAKE_ARGS=	CCFLAGS="${CFLAGS}"

PLIST_FILES=	sbin/ods2
PORTDOCS=	aareadme.too aareadme.txt

OPTIONS_DEFINE=	DOCS

post-patch:
	@${REINPLACE_CMD} -e 's|cc |${CC} |; /-oods2/ s|vmstime\.o|& -lcompat|' \
		${WRKSRC}/${MAKEFILE}

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/ods2 ${STAGEDIR}${PREFIX}/sbin

do-install-DOCS-on:
	@${MKDIR} ${STAGEDIR}${DOCSDIR}
	${INSTALL_DATA} ${PORTDOCS:S|^|${WRKSRC}/|} ${STAGEDIR}${DOCSDIR}

.include <bsd.port.mk>
