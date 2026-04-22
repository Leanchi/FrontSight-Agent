FROM --platform=linux/amd64 debian:10-slim

# Debian 10 (Buster) has been moved to archive
RUN sed -i 's|http://deb.debian.org/debian|http://archive.debian.org/debian|g' /etc/apt/sources.list && \
    sed -i 's|http://security.debian.org/debian-security|http://archive.debian.org/debian-security|g' /etc/apt/sources.list && \
    sed -i '/buster-updates/d' /etc/apt/sources.list

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev \
    libx11-dev libxtst-dev libxext-dev libjpeg62-turbo-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . /build/

RUN make linux ARCHID=6 CEXTRA="-DBLUR_DEBUG"
