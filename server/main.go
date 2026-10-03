package main

import (
	"crypto/sha256"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

// Keep this in sync with the OTA slot size in partitions.csv.
const maxFirmwareSize = 0x1f0000

func firmwareHandler(filename string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		if r.URL.Path != "/firmware" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Allow", "OPTIONS, GET")
		if r.Method != http.MethodOptions && r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		// Read a bounded snapshot: its ETag and response bytes must agree even
		// when the on-disk file is atomically replaced during this request.
		data, err := readFirmware(filename)
		if err != nil {
			log.Printf("firmware unavailable: %v", err)
			http.Error(w, "firmware unavailable", http.StatusServiceUnavailable)
			return
		}
		etag := fmt.Sprintf(`"%x"`, sha256.Sum256(data))
		w.Header().Set("ETag", etag)
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		if condition := r.Header.Get("If-Match"); condition != "" && !matches(condition, etag) {
			w.WriteHeader(http.StatusPreconditionFailed)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", strconv.Itoa(len(data)))
		if _, err := w.Write(data); err != nil {
			log.Printf("firmware download interrupted: %v", err)
		}
	})
}

func readFirmware(filename string) ([]byte, error) {
	f, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, maxFirmwareSize+1))
	if err != nil {
		return nil, err
	}
	if len(data) == 0 || len(data) > maxFirmwareSize {
		return nil, fmt.Errorf("firmware size must be between 1 and %d bytes", maxFirmwareSize)
	}
	return data, nil
}

func matches(condition, etag string) bool {
	for _, candidate := range strings.Split(condition, ",") {
		if value := strings.TrimSpace(candidate); value == "*" || value == etag {
			return true
		}
	}
	return false
}

func main() {
	addr := flag.String("addr", ":8080", "HTTP listen address (behind Traefik)")
	firmware := flag.String("firmware", "releases/firmware.bin", "published application binary")
	flag.Parse()
	server := &http.Server{
		Addr:              *addr,
		Handler:           firmwareHandler(*firmware),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      150 * time.Second,
		IdleTimeout:       30 * time.Second,
	}
	log.Printf("serving %s at %s/firmware", *firmware, *addr)
	log.Fatal(server.ListenAndServe())
}
