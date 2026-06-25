/** @type {import('next').NextConfig} */
const withPWA = require('next-pwa')({
  dest: 'public',
  register: true,
  skipWaiting: true,
  disable: process.env.NODE_ENV === 'development',
  buildExcludes: [/middleware-manifest\.json$/],
})

const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''

const nextConfig = {
  output: 'standalone',
  reactStrictMode: true,
  swcMinify: true,
  ...(proxyPath ? { basePath: proxyPath } : {}),
}

module.exports = withPWA(nextConfig)
