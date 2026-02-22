package com.hunter.app;

import android.util.Log;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFuture;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.socksx.SocksPortUnificationServerHandler;
import io.netty.handler.logging.LogLevel;
import io.netty.handler.logging.LoggingHandler;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Proxy balancer that provides SOCKS5 servers on ports 10812 and 10813.
 * Port 10812: General balancer for all good configs
 * Port 10813: Google balancer for configs with Google access
 */
public class ProxyBalancer {
    private static final String TAG = "ProxyBalancer";
    private static final int GENERAL_PORT = 10812;
    private static final int GOOGLE_PORT = 10813;
    private static final int BACKEND_BASE_PORT = 20808;
    private static final int MAX_BACKENDS = 3;

    private final ConfigBalancer generalBalancer;
    private final ConfigBalancer googleBalancer;

    private final ExecutorService executor;
    private EventLoopGroup bossGroup;
    private EventLoopGroup workerGroup;

    private Channel generalChannel;
    private Channel googleChannel;

    private List<Backend> generalBackends;
    private List<Backend> googleBackends;

    private volatile boolean isRunning = false;
    private final AtomicInteger backendPortCounter = new AtomicInteger(BACKEND_BASE_PORT);

    public ProxyBalancer() {
        this.generalBalancer = new ConfigBalancer();
        this.googleBalancer = new ConfigBalancer();
        this.executor = Executors.newCachedThreadPool();
        this.generalBackends = new ArrayList<>();
        this.googleBackends = new ArrayList<>();
    }

    /**
     * Start the proxy balancers
     */
    public synchronized void start() {
        if (isRunning) {
            Log.w(TAG, "Proxy balancers already running");
            return;
        }

        try {
            bossGroup = new NioEventLoopGroup(1);
            workerGroup = new NioEventLoopGroup();

            // Start backends
            startBackends();

            // Start general balancer on port 10812
            ServerBootstrap generalBootstrap = createServerBootstrap();
            generalBootstrap.childHandler(new ChannelInitializer<SocketChannel>() {
                @Override
                protected void initChannel(SocketChannel ch) throws Exception {
                    ch.pipeline().addLast(new LoggingHandler(LogLevel.DEBUG));
                    ch.pipeline().addLast(new SocksPortUnificationServerHandler());
                    ch.pipeline().addLast(new LoadBalancingHandler(generalBackends));
                }
            });

            ChannelFuture generalFuture = generalBootstrap.bind(GENERAL_PORT).sync();
            generalChannel = generalFuture.channel();
            Log.i(TAG, "General balancer started on port " + GENERAL_PORT);

            // Start Google balancer on port 10813
            ServerBootstrap googleBootstrap = createServerBootstrap();
            googleBootstrap.childHandler(new ChannelInitializer<SocketChannel>() {
                @Override
                protected void initChannel(SocketChannel ch) throws Exception {
                    ch.pipeline().addLast(new LoggingHandler(LogLevel.DEBUG));
                    ch.pipeline().addLast(new SocksPortUnificationServerHandler());
                    ch.pipeline().addLast(new LoadBalancingHandler(googleBackends));
                }
            });

            ChannelFuture googleFuture = googleBootstrap.bind(GOOGLE_PORT).sync();
            googleChannel = googleFuture.channel();
            Log.i(TAG, "Google balancer started on port " + GOOGLE_PORT);

            isRunning = true;
            Log.i(TAG, "Proxy balancers started successfully");

        } catch (Exception e) {
            Log.e(TAG, "Failed to start proxy balancers", e);
            stop();
        }
    }

    /**
     * Stop the proxy balancers
     */
    public synchronized void stop() {
        if (!isRunning) {
            return;
        }

        isRunning = false;

        try {
            if (generalChannel != null) {
                generalChannel.close().sync();
            }
            if (googleChannel != null) {
                googleChannel.close().sync();
            }
        } catch (Exception e) {
            Log.w(TAG, "Error closing channels", e);
        }

        // Stop backends
        stopBackends();

        if (bossGroup != null) {
            bossGroup.shutdownGracefully();
        }
        if (workerGroup != null) {
            workerGroup.shutdownGracefully();
        }

        executor.shutdown();
        Log.i(TAG, "Proxy balancers stopped");
    }

    /**
     * Update configs for the balancers
     */
    public void updateConfigs(List<ConfigManager.ConfigItem> allConfigs) {
        // Separate configs into general and Google-accessible
        List<ConfigManager.ConfigItem> generalConfigs = new ArrayList<>();
        List<ConfigManager.ConfigItem> googleConfigs = new ArrayList<>();

        for (ConfigManager.ConfigItem config : allConfigs) {
            if (config.latency > 0 && config.latency < 10000) { // Working configs
                generalConfigs.add(config);
                if (config.googleAccessible) {
                    googleConfigs.add(config);
                }
            }
        }

        generalBalancer.updateConfigs(generalConfigs);
        googleBalancer.updateConfigs(googleConfigs);

        // Update backends with new configs
        updateBackends(generalBackends, generalConfigs);
        updateBackends(googleBackends, googleConfigs);

        Log.i(TAG, "Updated balancers - General: " + generalConfigs.size() +
              ", Google: " + googleConfigs.size());
    }

    /**
     * Check if balancers are running
     */
    public boolean isRunning() {
        return isRunning;
    }

    /**
     * Get status of balancers
     */
    public String getStatus() {
        return String.format("General(port %d): %d backends, Google(port %d): %d backends",
            GENERAL_PORT, generalBackends.size(),
            GOOGLE_PORT, googleBackends.size());
    }

    private void startBackends() {
        // Start general backends
        List<ConfigManager.ConfigItem> generalConfigs = generalBalancer.getWorkingConfigCount() > 0 ?
            generalBalancer.getMultipleConfigs(MAX_BACKENDS) : new ArrayList<>();
        for (ConfigManager.ConfigItem config : generalConfigs) {
            Backend backend = new Backend(config, backendPortCounter.getAndIncrement());
            if (backend.start()) {
                generalBackends.add(backend);
            }
        }

        // Start Google backends
        List<ConfigManager.ConfigItem> googleConfigs = googleBalancer.getWorkingConfigCount() > 0 ?
            googleBalancer.getMultipleConfigs(MAX_BACKENDS) : new ArrayList<>();
        for (ConfigManager.ConfigItem config : googleConfigs) {
            Backend backend = new Backend(config, backendPortCounter.getAndIncrement());
            if (backend.start()) {
                googleBackends.add(backend);
            }
        }
    }

    private void stopBackends() {
        for (Backend backend : generalBackends) {
            backend.stop();
        }
        generalBackends.clear();

        for (Backend backend : googleBackends) {
            backend.stop();
        }
        googleBackends.clear();
    }

    private void updateBackends(List<Backend> backends, List<ConfigManager.ConfigItem> newConfigs) {
        // Simple update: stop all and restart with new configs
        // In production, could implement more sophisticated update logic
        for (Backend backend : backends) {
            backend.stop();
        }
        backends.clear();

        int count = Math.min(newConfigs.size(), MAX_BACKENDS);
        for (int i = 0; i < count; i++) {
            Backend backend = new Backend(newConfigs.get(i), backendPortCounter.getAndIncrement());
            if (backend.start()) {
                backends.add(backend);
            }
        }
    }

    private ServerBootstrap createServerBootstrap() {
        ServerBootstrap b = new ServerBootstrap();
        b.group(bossGroup, workerGroup)
         .channel(NioServerSocketChannel.class)
         .childOption(ChannelOption.SO_KEEPALIVE, true);
        return b;
    }
}
