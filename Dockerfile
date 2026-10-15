# CoreRealtime server image (Linux x86_64).
#   docker buildx build --platform linux/amd64 -t corerealtime .
# Run:
#   docker run -d --name corerealtime --restart unless-stopped \
#     -p 5055-5058:5055-5058/udp -p 27000-27002:27000-27002/udp \
#     -p 4530:4530/tcp -p 4531:4531/tcp -p 4533:4533/tcp \
#     -v ./config.yml:/srv/config.yml:ro \
#     corerealtime

ARG ARCH=amd64

FROM --platform=linux/${ARCH} ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git libmbedtls-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

FROM --platform=linux/${ARCH} ubuntu:24.04 AS runtime
WORKDIR /srv
COPY --from=build /src/build/corerealtime /srv/corerealtime
COPY config.example.yml /srv/config.yml
EXPOSE 5055-5058/udp 27000-27002/udp
EXPOSE 4530/tcp 4531/tcp 4533/tcp
ENTRYPOINT ["/srv/corerealtime"]
CMD ["/srv/config.yml"]
