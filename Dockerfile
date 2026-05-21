# syntax=docker/dockerfile:1

FROM --platform=$BUILDPLATFORM gcc:13-bookworm AS build
WORKDIR /src

COPY Makefile ./
COPY include ./include
COPY src ./src

RUN make clean && make

FROM debian:bookworm-slim AS runtime
WORKDIR /app

COPY --from=build /src/build/rinha-api /app/rinha-api

ENV RINHA_ADDR=:8080
EXPOSE 8080

USER 65532:65532
ENTRYPOINT ["/app/rinha-api"]
