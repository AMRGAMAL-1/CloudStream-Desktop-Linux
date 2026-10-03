package android.util;

import java.nio.charset.StandardCharsets;

/**
 * Port of android.util.Base64 (AOSP, Apache License 2.0) with identical semantics for all flags,
 * including the 76 character line wrapping and trailing newline of DEFAULT, and the lenient decoder.
 */
public class Base64 {
    public static final int DEFAULT = 0;
    public static final int NO_PADDING = 1;
    public static final int NO_WRAP = 2;
    public static final int CRLF = 4;
    public static final int URL_SAFE = 8;
    public static final int NO_CLOSE = 16;

    static abstract class Coder {
        public byte[] output;
        public int op;

        public abstract boolean process(byte[] input, int offset, int len, boolean finish);

        public abstract int maxOutputSize(int len);
    }

    public static byte[] decode(String str, int flags) {
        return decode(str.getBytes(), flags);
    }

    public static byte[] decode(byte[] input, int flags) {
        return decode(input, 0, input.length, flags);
    }

    public static byte[] decode(byte[] input, int offset, int len, int flags) {
        Decoder decoder = new Decoder(flags, new byte[len * 3 / 4]);
        if (!decoder.process(input, offset, len, true)) {
            throw new IllegalArgumentException("bad base-64");
        }
        if (decoder.op == decoder.output.length) {
            return decoder.output;
        }
        byte[] temp = new byte[decoder.op];
        System.arraycopy(decoder.output, 0, temp, 0, decoder.op);
        return temp;
    }

    static class Decoder extends Coder {
        private static final int[] DECODE = new int[256];
        private static final int[] DECODE_WEBSAFE = new int[256];

        static {
            java.util.Arrays.fill(DECODE, -1);
            java.util.Arrays.fill(DECODE_WEBSAFE, -1);
            String std = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            String web = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            for (int i = 0; i < 64; i++) {
                DECODE[std.charAt(i)] = i;
                DECODE_WEBSAFE[web.charAt(i)] = i;
            }
            DECODE['='] = -2;
            DECODE_WEBSAFE['='] = -2;
        }

        private static final int SKIP = -1;
        private static final int EQUALS = -2;
        private int state;
        private int value;
        final private int[] alphabet;

        public Decoder(int flags, byte[] output) {
            this.output = output;
            alphabet = ((flags & URL_SAFE) == 0) ? DECODE : DECODE_WEBSAFE;
            state = 0;
            value = 0;
        }

        public int maxOutputSize(int len) {
            return len * 3 / 4 + 10;
        }

        public boolean process(byte[] input, int offset, int len, boolean finish) {
            if (this.state == 6) return false;
            int p = offset;
            len += offset;
            int state = this.state;
            int value = this.value;
            int op = 0;
            final byte[] output = this.output;
            final int[] alphabet = this.alphabet;
            while (p < len) {
                if (state == 0) {
                    while (p + 4 <= len &&
                            (value = ((alphabet[input[p] & 0xff] << 18) |
                                    (alphabet[input[p + 1] & 0xff] << 12) |
                                    (alphabet[input[p + 2] & 0xff] << 6) |
                                    (alphabet[input[p + 3] & 0xff]))) >= 0) {
                        output[op + 2] = (byte) value;
                        output[op + 1] = (byte) (value >> 8);
                        output[op] = (byte) (value >> 16);
                        op += 3;
                        p += 4;
                    }
                    if (p >= len) break;
                }
                int d = alphabet[input[p++] & 0xff];
                switch (state) {
                    case 0:
                        if (d >= 0) {
                            value = d;
                            ++state;
                        } else if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                    case 1:
                        if (d >= 0) {
                            value = (value << 6) | d;
                            ++state;
                        } else if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                    case 2:
                        if (d >= 0) {
                            value = (value << 6) | d;
                            ++state;
                        } else if (d == EQUALS) {
                            output[op++] = (byte) (value >> 4);
                            state = 4;
                        } else if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                    case 3:
                        if (d >= 0) {
                            value = (value << 6) | d;
                            output[op + 2] = (byte) value;
                            output[op + 1] = (byte) (value >> 8);
                            output[op] = (byte) (value >> 16);
                            op += 3;
                            state = 0;
                        } else if (d == EQUALS) {
                            output[op + 1] = (byte) (value >> 2);
                            output[op] = (byte) (value >> 10);
                            op += 2;
                            state = 5;
                        } else if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                    case 4:
                        if (d == EQUALS) {
                            ++state;
                        } else if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                    case 5:
                        if (d != SKIP) {
                            this.state = 6;
                            return false;
                        }
                        break;
                }
            }
            if (!finish) {
                this.state = state;
                this.value = value;
                this.op = op;
                return true;
            }
            switch (state) {
                case 0:
                    break;
                case 1:
                    this.state = 6;
                    return false;
                case 2:
                    output[op++] = (byte) (value >> 4);
                    break;
                case 3:
                    output[op++] = (byte) (value >> 10);
                    output[op++] = (byte) (value >> 2);
                    break;
                case 4:
                    this.state = 6;
                    return false;
                case 5:
                    break;
            }
            this.state = state;
            this.op = op;
            return true;
        }
    }

    public static String encodeToString(byte[] input, int flags) {
        return new String(encode(input, flags), StandardCharsets.US_ASCII);
    }

    public static String encodeToString(byte[] input, int offset, int len, int flags) {
        return new String(encode(input, offset, len, flags), StandardCharsets.US_ASCII);
    }

    public static byte[] encode(byte[] input, int flags) {
        return encode(input, 0, input.length, flags);
    }

    public static byte[] encode(byte[] input, int offset, int len, int flags) {
        Encoder encoder = new Encoder(flags, null);
        int output_len = len / 3 * 4;
        if (encoder.do_padding) {
            if (len % 3 > 0) {
                output_len += 4;
            }
        } else {
            switch (len % 3) {
                case 0:
                    break;
                case 1:
                    output_len += 2;
                    break;
                case 2:
                    output_len += 3;
                    break;
            }
        }
        if (encoder.do_newline && len > 0) {
            output_len += (((len - 1) / (3 * Encoder.LINE_GROUPS)) + 1) * (encoder.do_cr ? 2 : 1);
        }
        encoder.output = new byte[output_len];
        encoder.process(input, offset, len, true);
        return encoder.output;
    }

    static class Encoder extends Coder {
        public static final int LINE_GROUPS = 19;
        private static final byte[] ENCODE =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".getBytes(StandardCharsets.US_ASCII);
        private static final byte[] ENCODE_WEBSAFE =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".getBytes(StandardCharsets.US_ASCII);
        final private byte[] tail;
        int tailLen;
        private int count;
        final public boolean do_padding;
        final public boolean do_newline;
        final public boolean do_cr;
        final private byte[] alphabet;

        public Encoder(int flags, byte[] output) {
            this.output = output;
            do_padding = (flags & NO_PADDING) == 0;
            do_newline = (flags & NO_WRAP) == 0;
            do_cr = (flags & CRLF) != 0;
            alphabet = ((flags & URL_SAFE) == 0) ? ENCODE : ENCODE_WEBSAFE;
            tail = new byte[2];
            tailLen = 0;
            count = do_newline ? LINE_GROUPS : -1;
        }

        public int maxOutputSize(int len) {
            return len * 8 / 5 + 10;
        }

        public boolean process(byte[] input, int offset, int len, boolean finish) {
            final byte[] alphabet = this.alphabet;
            final byte[] output = this.output;
            int op = 0;
            int count = this.count;
            int p = offset;
            len += offset;
            int v = -1;
            switch (tailLen) {
                case 0:
                    break;
                case 1:
                    if (p + 2 <= len) {
                        v = ((tail[0] & 0xff) << 16) | ((input[p++] & 0xff) << 8) | (input[p++] & 0xff);
                        tailLen = 0;
                    }
                    break;
                case 2:
                    if (p + 1 <= len) {
                        v = ((tail[0] & 0xff) << 16) | ((tail[1] & 0xff) << 8) | (input[p++] & 0xff);
                        tailLen = 0;
                    }
                    break;
            }
            if (v != -1) {
                output[op++] = alphabet[(v >> 18) & 0x3f];
                output[op++] = alphabet[(v >> 12) & 0x3f];
                output[op++] = alphabet[(v >> 6) & 0x3f];
                output[op++] = alphabet[v & 0x3f];
                if (--count == 0) {
                    if (do_cr) output[op++] = '\r';
                    output[op++] = '\n';
                    count = LINE_GROUPS;
                }
            }
            while (p + 3 <= len) {
                v = ((input[p] & 0xff) << 16) | ((input[p + 1] & 0xff) << 8) | (input[p + 2] & 0xff);
                output[op] = alphabet[(v >> 18) & 0x3f];
                output[op + 1] = alphabet[(v >> 12) & 0x3f];
                output[op + 2] = alphabet[(v >> 6) & 0x3f];
                output[op + 3] = alphabet[v & 0x3f];
                p += 3;
                op += 4;
                if (--count == 0) {
                    if (do_cr) output[op++] = '\r';
                    output[op++] = '\n';
                    count = LINE_GROUPS;
                }
            }
            if (finish) {
                if (p - tailLen == len - 1) {
                    int t = 0;
                    v = ((tailLen > 0 ? tail[t++] : input[p++]) & 0xff) << 4;
                    tailLen -= t;
                    output[op++] = alphabet[(v >> 6) & 0x3f];
                    output[op++] = alphabet[v & 0x3f];
                    if (do_padding) {
                        output[op++] = '=';
                        output[op++] = '=';
                    }
                    if (do_newline) {
                        if (do_cr) output[op++] = '\r';
                        output[op++] = '\n';
                    }
                } else if (p - tailLen == len - 2) {
                    int t = 0;
                    v = (((tailLen > 1 ? tail[t++] : input[p++]) & 0xff) << 10) |
                            (((tailLen > 0 ? tail[t++] : input[p++]) & 0xff) << 2);
                    tailLen -= t;
                    output[op++] = alphabet[(v >> 12) & 0x3f];
                    output[op++] = alphabet[(v >> 6) & 0x3f];
                    output[op++] = alphabet[v & 0x3f];
                    if (do_padding) {
                        output[op++] = '=';
                    }
                    if (do_newline) {
                        if (do_cr) output[op++] = '\r';
                        output[op++] = '\n';
                    }
                } else if (do_newline && op > 0 && count != LINE_GROUPS) {
                    if (do_cr) output[op++] = '\r';
                    output[op++] = '\n';
                }
            } else {
                if (p == len - 1) {
                    tail[tailLen++] = input[p];
                } else if (p == len - 2) {
                    tail[tailLen++] = input[p];
                    tail[tailLen++] = input[p + 1];
                }
            }
            this.op = op;
            this.count = count;
            return true;
        }
    }

    private Base64() {
    }
}
