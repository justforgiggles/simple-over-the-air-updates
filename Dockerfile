FROM golang:1.25-alpine AS build
WORKDIR /src
COPY server/go.mod ./
COPY server/*.go ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /sotau .

FROM scratch
COPY --from=build /sotau /sotau
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/sotau"]
CMD ["-addr=:8080", "-firmware=/releases/firmware.bin"]
