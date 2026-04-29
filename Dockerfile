FROM alpine:3.18 AS builder

RUN apk add --no-progress --no-cache \
    linux-headers gcc g++ clang-dev make cmake bash \
    mariadb-dev mariadb-static postgresql-dev sqlite-dev sqlite-static\
    openssl-dev openssl-libs-static zlib-dev zlib-static icu-dev icu-static \
    pcre-dev bison git musl-dev libelf-static elfutils-dev zstd-static bzip2-static xz-static

WORKDIR /build

RUN wget -O - https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2 | tar -xj

WORKDIR /build/jemalloc-5.3.0

RUN ./configure --prefix=/usr \
    && make \
    && make install

COPY . /build/mudos-ng
RUN mkdir /build/mudos-ng/build

WORKDIR /build/mudos-ng/build
RUN cmake .. -DMARCH_NATIVE=OFF -DSTATIC=ON \
    && make install

FROM alpine:3.18

RUN apk add --no-progress --no-cache \
    icu-data-full

WORKDIR /mudos-ng

COPY --from=builder /build/mudos-ng/build/bin ./bin

ENTRYPOINT ["/mudos-ng/bin/driver"]
