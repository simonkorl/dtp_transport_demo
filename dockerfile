FROM simonkorl0228/aitrans_build:buster as build
WORKDIR /build
COPY . ./
WORKDIR /build/examples/ping-pong
RUN make

FROM simonkorl0228/aitrans_image_base:buster
WORKDIR /
COPY --from=build \
    /build/examples/ping-pong/server ./server
COPY --from=build \
    /build/examples/ping-pong/client ./client
COPY --from=build \
    /build/examples/ping-pong/cert.crt cert.crt
COPY --from=build \
    /build/examples/ping-pong/cert.key cert.key