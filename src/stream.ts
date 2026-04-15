import { Duplex, type DuplexOptions } from "node:stream";

/**
 * The native stream binding exposed by the Rust addon.
 * This interface mirrors the napi-rs class methods.
 */
export interface NativeStreamBinding {
  /**
   * Register a callback that the native read thread invokes (via
   * ThreadsafeFunction) when data arrives from the COM side.
   */
  onData(callback: (chunk: Buffer) => void): void;

  /**
   * Register a callback for read-side errors.
   * Receives an error message string (not an Error object).
   */
  onReadError(callback: (message: string) => void): void;

  /**
   * Write a buffer to the companion device. The callback is invoked
   * when the overlapped write completes.
   */
  write(chunk: Buffer, callback: (error: Error | null) => void): void;

  /**
   * Pause the native read thread. Used for backpressure — when the
   * JS readable side is full, we stop reading from the driver until
   * _read() is called again.
   */
  pauseReading(): void;

  /**
   * Resume the native read thread after a pause.
   */
  resumeReading(): void;

  /**
   * Shut down the native stream — cancel overlapped I/O, join
   * threads, close the handle.
   */
  shutdown(): void;
}

/**
 * A high-performance Duplex stream connected to a virtual COM port.
 *
 * Data written to this stream appears as received data on the COM
 * port (visible to any external application that opened it). Data
 * sent by the external application to the COM port is readable from
 * this stream.
 *
 * The byte stream is agnostic to serial line settings (baud rate,
 * parity, etc.). All bytes flow through at native speed regardless
 * of what the COM-side application configured.
 */
export class VirtualPortStream extends Duplex {
  readonly #native: NativeStreamBinding;
  #reading = false;

  constructor(native: NativeStreamBinding, options?: DuplexOptions) {
    super({
      // Raw bytes — no encoding transformation
      readableHighWaterMark: 64 * 1024,
      writableHighWaterMark: 64 * 1024,
      ...options,
    });

    this.#native = native;

    // The native read thread pushes data into the JS stream.
    native.onData((chunk: Buffer) => {
      if (!this.push(chunk)) {
        // Backpressure: the readable buffer is full.
        native.pauseReading();
        this.#reading = false;
      }
    });

    native.onReadError((message: string) => {
      this.destroy(new Error(message));
    });
  }

  /**
   * Called by the stream machinery when the consumer is ready for
   * more data. Resumes the native read thread if it was paused.
   */
  override _read(_size: number): void {
    if (!this.#reading) {
      this.#reading = true;
      this.#native.resumeReading();
    }
  }

  /**
   * Called by the stream machinery when the producer writes data.
   * Routes bytes to the companion device via overlapped WriteFile.
   */
  override _write(
    chunk: Buffer,
    _encoding: BufferEncoding,
    callback: (error?: Error | null) => void,
  ): void {
    this.#native.write(chunk, callback);
  }

  /**
   * Clean shutdown: cancel pending I/O, join native threads,
   * close the companion handle.
   */
  override _destroy(
    error: Error | null,
    callback: (error?: Error | null) => void,
  ): void {
    this.#native.shutdown();
    callback(error);
  }
}
