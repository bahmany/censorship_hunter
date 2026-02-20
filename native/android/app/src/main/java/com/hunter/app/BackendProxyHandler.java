package com.hunter.app;

import android.util.Log;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.handler.codec.socksx.v5.*;

/**
 * Handler that proxies SOCKS connections to backend servers
 */
public class BackendProxyHandler extends ChannelInboundHandlerAdapter {
    private static final String TAG = "BackendProxyHandler";

    private final ChannelHandlerContext clientCtx;
    private final Socks5CommandRequest originalRequest;
    private boolean greetingSent = false;
    private boolean authReceived = false;
    private boolean commandSent = false;

    public BackendProxyHandler(ChannelHandlerContext clientCtx, Socks5CommandRequest request) {
        this.clientCtx = clientCtx;
        this.originalRequest = request;
    }

    @Override
    public void channelActive(ChannelHandlerContext backendCtx) throws Exception {
        // Send SOCKS5 greeting to backend
        ByteBuf greeting = Unpooled.buffer();
        greeting.writeByte(0x05); // VER
        greeting.writeByte(0x01); // NMETHODS
        greeting.writeByte(0x00); // NO AUTH
        backendCtx.writeAndFlush(greeting);
        greetingSent = true;
        Log.d(TAG, "Sent SOCKS5 greeting to backend");
    }

    @Override
    public void channelRead(ChannelHandlerContext backendCtx, Object msg) throws Exception {
        if (msg instanceof ByteBuf) {
            ByteBuf buf = (ByteBuf) msg;

            if (!authReceived) {
                // Handle auth response
                if (buf.readableBytes() >= 2) {
                    int ver = buf.readByte();
                    int method = buf.readByte();

                    if (ver == 0x05 && method == 0x00) {
                        authReceived = true;
                        Log.d(TAG, "Received SOCKS5 auth response from backend");

                        // Send CONNECT command to backend
                        ByteBuf connectBuf = Unpooled.buffer();
                        connectBuf.writeByte(0x05); // VER
                        connectBuf.writeByte(0x01); // CMD=CONNECT
                        connectBuf.writeByte(0x00); // RSV

                        // Address type
                        if (originalRequest.dstAddrType() == Socks5AddressType.IPv4) {
                            connectBuf.writeByte(0x01); // IPv4
                            byte[] addr = originalRequest.dstAddr().getBytes();
                            connectBuf.writeBytes(addr);
                        } else if (originalRequest.dstAddrType() == Socks5AddressType.DOMAIN) {
                            connectBuf.writeByte(0x03); // DOMAIN
                            byte[] domain = originalRequest.dstAddr().getBytes();
                            connectBuf.writeByte(domain.length);
                            connectBuf.writeBytes(domain);
                        } else if (originalRequest.dstAddrType() == Socks5AddressType.IPv6) {
                            connectBuf.writeByte(0x04); // IPv6
                            byte[] addr = originalRequest.dstAddr().getBytes();
                            connectBuf.writeBytes(addr);
                        }

                        // Port
                        int port = originalRequest.dstPort();
                        connectBuf.writeByte((port >> 8) & 0xFF);
                        connectBuf.writeByte(port & 0xFF);

                        backendCtx.writeAndFlush(connectBuf);
                        commandSent = true;
                        Log.d(TAG, "Sent CONNECT command to backend: " +
                              originalRequest.dstAddr() + ":" + port);
                    } else {
                        Log.w(TAG, "Invalid SOCKS5 auth response: ver=" + ver + ", method=" + method);
                        closeBoth();
                    }
                }
            } else if (!commandSent) {
                // This shouldn't happen
                Log.w(TAG, "Received data before sending command");
            } else {
                // Handle command response
                if (buf.readableBytes() >= 10) {
                    int ver = buf.readByte();
                    int rep = buf.readByte();
                    buf.readByte(); // RSV
                    int atyp = buf.readByte();

                    if (ver == 0x05) {
                        Socks5CommandStatus status = Socks5CommandStatus.valueOf((byte)rep);
                        Log.d(TAG, "Received CONNECT response from backend: " + status);

                        // Send response to client
                        DefaultSocks5CommandResponse response =
                            new DefaultSocks5CommandResponse(status, originalRequest.dstAddrType());
                        clientCtx.writeAndFlush(response);

                        if (status == Socks5CommandStatus.SUCCESS) {
                            // Skip BND.ADDR and BND.PORT (assume IPv4 for simplicity)
                            buf.skipBytes(6);

                            // Now proxy data between client and backend
                            // Remove this handler and add a simple relay handler
                            backendCtx.pipeline().remove(this);
                            clientCtx.pipeline().addLast(new RelayHandler(backendCtx.channel()));

                            backendCtx.pipeline().addLast(new RelayHandler(clientCtx.channel()));

                            Log.d(TAG, "Proxy setup complete, starting data relay");
                        } else {
                            closeBoth();
                        }
                    } else {
                        Log.w(TAG, "Invalid SOCKS5 command response version: " + ver);
                        closeBoth();
                    }
                }
            }

            buf.release();
        } else {
            // Pass other messages
            clientCtx.fireChannelRead(msg);
        }
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "Exception in backend proxy handler", cause);
        closeBoth();
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) throws Exception {
        Log.d(TAG, "Backend channel inactive");
        closeBoth();
    }

    private void closeBoth() {
        try {
            if (clientCtx.channel().isActive()) {
                clientCtx.close();
            }
        } catch (Exception e) {
            Log.w(TAG, "Error closing client channel", e);
        }

        try {
            if (clientCtx.channel().isActive()) {
                clientCtx.close();
            }
        } catch (Exception e) {
            Log.w(TAG, "Error closing backend channel", e);
        }
    }

    /**
     * Simple relay handler for bidirectional data forwarding
     */
    private static class RelayHandler extends ChannelInboundHandlerAdapter {
        private final Channel peer;

        public RelayHandler(Channel peer) {
            this.peer = peer;
        }

        @Override
        public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
            if (peer.isActive()) {
                peer.writeAndFlush(msg);
            } else {
                // Peer closed, close this channel too
                ctx.close();
            }
        }

        @Override
        public void channelInactive(ChannelHandlerContext ctx) throws Exception {
            if (peer.isActive()) {
                peer.close();
            }
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
            Log.e(TAG, "Exception in relay handler", cause);
            ctx.close();
        }
    }
}
