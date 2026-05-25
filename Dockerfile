# syntax=docker/dockerfile:1

FROM --platform=$BUILDPLATFORM gcc:13-bookworm AS build
WORKDIR /src
ARG MAKE_TARGET=all
ARG CFLAGS_PROFILE=current
ARG RINHA_ENABLE_METRICS=1

COPY Makefile ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY tools ./tools

RUN make clean && make ${MAKE_TARGET} CFLAGS_PROFILE=${CFLAGS_PROFILE} RINHA_ENABLE_METRICS=${RINHA_ENABLE_METRICS}

FROM build AS test
RUN make test
CMD ["make", "test"]

FROM debian:bookworm-slim AS runtime
WORKDIR /app

COPY --from=build /src/build/rinha-api /app/rinha-api

ENV RINHA_ADDR=:8080
EXPOSE 8080

USER 65532:65532
ENTRYPOINT ["/app/rinha-api"]
