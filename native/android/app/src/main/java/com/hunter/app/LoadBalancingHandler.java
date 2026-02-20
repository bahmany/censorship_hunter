package com.hunter.app;

import android.util.Log;

import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.socket.SocketChannel;
import io.netty.handler.codec.socksx.SocksVersion;
import io.netty.handler.codec.socksx.v5.*;

import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Load balancing handler that routes SOCKS5 connections to backends
 */
public class LoadBalancingHandler extends ChannelInboundHandlerAdapter {
    private static final String TAG = "LoadBalancingHandler";

    private final List<Backend> backends;
    private final AtomicInteger roundRobinIndex = new AtomicInteger(0);

    public LoadBalancingHandler(List<Backend> backends) {
        this.backends = backends;
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        if (msg instanceof Socks5CommandRequest) {
            Socks5CommandRequest request = (Socks5CommandRequest) msg;

            if (request.type() == Socks5CommandType.CONNECT) {
                // Select backend using round-robin
                Backend selectedBackend = selectBackend();
                if (selectedBackend == null || !selectedBackend.isRunning()) {
                    Log.w(TAG, "No available backend for request");
                    ctx.writeAndFlush(new DefaultSocks5CommandResponse(
                        Socks5CommandStatus.FAILURE, Socks5AddressType.IPv4));
                    ctx.close();
                    return;
                }

                // Establish connection to backend
                connectToBackend(ctx, request, selectedBackend);
            } else {
                // Only support CONNECT for now
                ctx.writeAndFlush(new DefaultSocks5CommandResponse(
                    Socks5CommandStatus.COMMAND_UNSUPPORTED, Socks5AddressType.IPv4));
                ctx.close();
            }
        } else {
            // Pass other messages (like auth requests)
            ctx.fireChannelRead(msg);
        }
    }

    private Backend selectBackend() {
        if (backends.isEmpty()) {
            return null;
        }

        // Round-robin selection
        int index = roundRobinIndex.getAndIncrement() % backends.size();
        Backend backend = backends.get(index);

        // Check if backend is still running
        if (!backend.isRunning()) {
            // Remove dead backend
            backends.remove(index);
            roundRobinIndex.set(0); // Reset index
            return selectBackend(); // Try again
        }

        return backend;
    }

    private void connectToBackend(ChannelHandlerContext ctx, Socks5CommandRequest request, Backend backend) {
        // Create connection to backend SOCKS port
        io.netty.bootstrap.Bootstrap bootstrap = new io.netty.bootstrap.Bootstrap();
        bootstrap.group(ctx.channel().eventLoop())
                 .channel(ctx.channel().getClass())
                 .handler(new ChannelInitializer<SocketChannel>() {
                     @Override
                     protected void initChannel(SocketChannel ch) throws Exception {
                         // Forward the same request to backend
                         ch.pipeline().addLast(new BackendProxyHandler(ctx, request));
                     }
                 });

        bootstrap.connect("127.0.0.1", backend.getPort()).addListener(future -> {
            if (future.isSuccess()) {
                Log.d(TAG, "Connected to backend on port " + backend.getPort());
                // Connection established, BackendProxyHandler will handle the forwarding
            } else {
                Log.w(TAG, "Failed to connect to backend on port " + backend.getPort(), future.cause());
                ctx.writeAndFlush(new DefaultSocks5CommandResponse(
                    Socks5CommandStatus.FAILURE, Socks5AddressType.IPv4));
                ctx.close();
            }
        });
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
        Log.e(TAG, "Exception in load balancing handler", cause);
        ctx.close();
    }
}
