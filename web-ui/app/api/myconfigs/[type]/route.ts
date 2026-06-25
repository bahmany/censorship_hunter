import { NextRequest, NextResponse } from 'next/server';
import fs from 'fs';
import path from 'path';

export const dynamic = 'force-dynamic';

const RUNTIME_PATHS = [
  '/app/backend-runtime',
  '/app/runtime',
  './runtime',
];

function findFile(filename: string): string | null {
  for (const dir of RUNTIME_PATHS) {
    const full = path.join(dir, filename);
    try {
      if (fs.existsSync(full) && fs.statSync(full).size > 0) {
        return full;
      }
    } catch {}
  }
  return null;
}

function readGoldConfigs(): string[] {
  const goldPath = findFile('HUNTER_gold.txt');
  if (goldPath) {
    const content = fs.readFileSync(goldPath, 'utf-8');
    return content.split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'));
  }
  return [];
}

function readAllConfigs(): string[] {
  const tsvPath = findFile('HUNTER_config_db.tsv');
  if (!tsvPath) return [];
  const content = fs.readFileSync(tsvPath, 'utf-8');
  const lines = content.split('\n');
  const configs: string[] = [];
  for (const line of lines) {
    if (line.startsWith('#') || !line.trim()) continue;
    const parts = line.split('\t');
    if (parts.length >= 1 && parts[0].trim()) {
      configs.push(parts[0].trim());
    }
  }
  return configs;
}

function readSilverConfigs(): string[] {
  const tsvPath = findFile('HUNTER_config_db.tsv');
  if (!tsvPath) return [];
  const content = fs.readFileSync(tsvPath, 'utf-8');
  const lines = content.split('\n');
  const configs: string[] = [];
  for (const line of lines) {
    if (line.startsWith('#') || !line.trim()) continue;
    const parts = line.split('\t');
    if (parts.length >= 7) {
      const alive = parseInt(parts[6], 10);
      if (alive === 0) {
        configs.push(parts[0].trim());
      }
    }
  }
  return configs;
}

function getGeneratedConfigs(): { name: string; uri: string; port: number; host: string }[] {
  return [
    { name: 'Abhar-VMess-1',       uri: 'vmess://eyJhZGQiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJhaWQiOiIwIiwiYWxwbiI6IiIsImZwIjoiY2hyb21lIiwiaG9zdCI6ImFwaS5hYmhhcmNhYmxlLmNvbSIsImlkIjoiZDYwMTFiZmEtMGRlMy00MTY5LTkxYjUtNzgxNmQyZjZkZDBiIiwibmV0Ijoid3MiLCJwYXRoIjoiL3hyYXkvdm1lc3MvIiwicG9ydCI6IjQ0MyIsInBzIjoiQWJoYXItVk1lc3MtMSIsInNjeSI6ImF1dG8iLCJzbmkiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJ0bHMiOiJ0bHMiLCJ0eXBlIjoibm9uZSIsInYiOiIyIn0=', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-Trojan-1',      uri: 'trojan://3f8a2c1b9e4d4a7f8b6c2d5e9f1a3b7c@api.abharcable.com:443?path=%2Fxray%2Ftrojan%2F&security=tls&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-Trojan-1', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VMess-2',       uri: 'vmess://eyJhZGQiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJhaWQiOiIwIiwiYWxwbiI6IiIsImZwIjoiY2hyb21lIiwiaG9zdCI6ImFwaS5hYmhhcmNhYmxlLmNvbSIsImlkIjoiOTZlMTBkNzctOWMwYy00NzNjLTllNzAtNGMzZmIxYmFhN2RjIiwibmV0Ijoid3MiLCJwYXRoIjoiL3hyYXkvdm1lc3MvIiwicG9ydCI6IjQ0MyIsInBzIjoiQWJoYXItVk1lc3MtMiIsInNjeSI6ImF1dG8iLCJzbmkiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJ0bHMiOiJ0bHMiLCJ0eXBlIjoibm9uZSIsInYiOiIyIn0=', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-Trojan-2',      uri: 'trojan://7a9d5e2f1b8c4d3e9a6f2b7c8e5d1f9a@api.abharcable.com:443?path=%2Fxray%2Ftrojan%2F&security=tls&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-Trojan-2', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-3',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_2%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-3', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-6',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_p%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-6', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-3',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_2%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-3', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-6',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_p%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-6', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-4',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_3%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-4', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-4',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_3%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-4', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-1',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-1', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-5',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_4%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-5', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-2',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_1%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-2', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-5',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_4%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-5', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-1-1',     uri: 'vless://d6011bfa-0de3-4169-91b5-7816d2f6dd0b@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-1-1', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-VLESS-2-2',     uri: 'vless://96e10d77-9c0c-473c-9e70-4c3fb1baa7dc@api.abharcable.com:443?path=%2Fws%2Fproxy%2Ftunnel_v2_1%2F&security=tls&alpn=http%2F1.1&encryption=none&host=api.abharcable.com&fp=chrome&type=ws&sni=api.abharcable.com#Abhar-VLESS-2-2', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-Shadowsocks-1', uri: 'ss://Y2hhY2hhMjAtaWV0Zi1wb2x5MTMwNTpBYmhhckNhYmxlMjAyNFNlY3VyZSE%3D@api.abharcable.com:443#Abhar-Shadowsocks-1', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-Hysteria2-1',   uri: 'hysteria2://71737668a9d19ee78af7098e@api.abharcable.com:443?security=tls&insecure=0&sni=api.abharcable.com#Abhar-Hysteria2-1', port: 443, host: 'api.abharcable.com' },
    { name: 'Abhar-Hysteria2-2',   uri: 'hysteria2://88af1a84a3415bb270793802@api.abharcable.com:443?security=tls&insecure=0&sni=api.abharcable.com#Abhar-Hysteria2-2', port: 443, host: 'api.abharcable.com' },
  ];
}

function getMetadata() {
  const gold = readGoldConfigs();
  const all = readAllConfigs();
  const generated = getGeneratedConfigs();
  return {
    generation_timestamp: new Date().toISOString(),
    epoch_timestamp: Math.floor(Date.now() / 1000),
    active_routes_count: gold.length,
    upstream_count: 2,
    backend_count: all.length,
    generated_count: generated.length,
    generated_active: generated.length,
  };
}

export async function GET(request: NextRequest) {
  const { pathname } = new URL(request.url);
  const type = pathname.split('/').pop() || 'all';

  const meta = getMetadata();

  if (type === 'metadata' || type === 'info') {
    return NextResponse.json(meta, { headers: { 'Cache-Control': 'no-store' } });
  }

  let configs: string[] = [];
  let filename = 'hunter_configs.txt';

  if (type === 'gold') {
    configs = readGoldConfigs();
    filename = 'hunter_gold_configs.txt';
  } else if (type === 'silver') {
    configs = readSilverConfigs();
    filename = 'hunter_silver_configs.txt';
  } else if (type === 'all') {
    configs = readAllConfigs();
    filename = 'hunter_all_configs.txt';
  } else if (type === 'generated') {
    const generated = getGeneratedConfigs();
    const content = `# Hunter Generated Configurations\n# Generated: ${meta.generation_timestamp}\n# Total: ${generated.length}\n# Active: ${generated.length}\n\n` +
      generated.map(c => c.uri).join('\n') + '\n';
    return new NextResponse(content, {
      headers: {
        'Content-Type': 'text/plain; charset=utf-8',
        'Content-Disposition': `attachment; filename="${filename}"`,
        'X-Config-Count': String(generated.length),
        'X-Generation-Timestamp': meta.generation_timestamp,
        'X-Active-Routes': String(meta.active_routes_count),
        'X-Upstream-Count': String(meta.upstream_count),
        'X-Backend-Count': String(meta.backend_count),
        'Cache-Control': 'no-store',
      },
    });
  } else if (type === 'texts') {
    const gold = readGoldConfigs();
    const silver = readSilverConfigs();
    const generated = getGeneratedConfigs();
    const all = readAllConfigs();
    const content = JSON.stringify({
      ...meta,
      gold_configs: gold,
      silver_configs: silver,
      generated_configs: generated.map(g => g.uri),
      all_count: all.length,
    }, null, 2);
    return new NextResponse(content, {
      headers: {
        'Content-Type': 'application/json',
        'X-Config-Count': String(gold.length + silver.length),
        'X-Generation-Timestamp': meta.generation_timestamp,
        'Cache-Control': 'no-store',
      },
    });
  } else {
    return NextResponse.json({ error: 'Unknown type. Use: all, gold, silver, generated, texts, metadata' }, { status: 400 });
  }

  if (configs.length === 0) {
    return NextResponse.json({
      error: 'No configurations found',
      ...meta,
    }, { status: 404, headers: { 'Cache-Control': 'no-store' } });
  }

  const content = configs.join('\n') + '\n';
  return new NextResponse(content, {
    headers: {
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Disposition': `attachment; filename="${filename}"`,
      'X-Config-Count': String(configs.length),
      'X-Generation-Timestamp': meta.generation_timestamp,
      'X-Active-Routes': String(meta.active_routes_count),
      'X-Upstream-Count': String(meta.upstream_count),
      'X-Backend-Count': String(meta.backend_count),
      'Cache-Control': 'no-store',
    },
  });
}
