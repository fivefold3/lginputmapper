const path = require('path');
const webpack = require('webpack');
const CopyPlugin = require('copy-webpack-plugin');

const pkg = require('./package.json');
const appinfo = require('./appinfo.json');
const servicePkg = require('./service/package.json');

// Emits a JSON asset into the compilation.
class JsonAssetPlugin {
  constructor(name, value) { this.name = name; this.value = value; }
  apply(compiler) {
    compiler.hooks.thisCompilation.tap('JsonAssetPlugin', (compilation) => {
      compilation.hooks.processAssets.tap(
        { name: 'JsonAssetPlugin', stage: webpack.Compilation.PROCESS_ASSETS_STAGE_ADDITIONAL },
        () => {
          const source = JSON.stringify(this.value, null, 2);
          compilation.emitAsset(this.name, new webpack.sources.RawSource(source));
        }
      );
    });
  }
}

const babelRule = { test: /\.m?js$/, exclude: /node_modules/, use: 'babel-loader' };

module.exports = (env = {}) => {
  const mode = env.production ? 'production' : 'development';
  const webApp = {
    name: 'app',
    mode,
    target: ['web', 'es5'],
    devtool: false,
    entry: { app: './frontend/index.js' },
    output: {
      path: path.resolve(__dirname, 'dist/app'),
      filename: '[name].js',
      clean: true,
    },
    module: { rules: [babelRule] },
    plugins: [
      new webpack.DefinePlugin({ __APP_VERSION__: JSON.stringify(pkg.version) }),
      new CopyPlugin({
        patterns: [
          { context: 'assets', from: '**/*' },
          { context: 'frontend', from: 'index.html' },
          { context: 'frontend', from: 'style.css' },
        ],
      }),
      new JsonAssetPlugin('appinfo.json', { ...appinfo, id: pkg.name, version: pkg.version }),
    ],
  };

  const service = {
    name: 'service',
    mode,
    target: 'node8',
    devtool: false,
    entry: { service: './service/service.js' },
    output: {
      path: path.resolve(__dirname, 'dist/service'),
      filename: '[name].js',
      // Keep dist/service/bin (daemon binaries) from the daemon build step.
      clean: { keep: /^bin\// },
    },
    externals: { 'webos-service': 'commonjs2 webos-service' },
    node: { __dirname: false, __filename: false },
    module: { rules: [babelRule] },
    plugins: [
      new webpack.DefinePlugin({ __APP_VERSION__: JSON.stringify(pkg.version) }),
      new JsonAssetPlugin('package.json', { ...servicePkg, version: pkg.version }),
      new JsonAssetPlugin('services.json', {
        id: servicePkg.name,
        description: servicePkg.description,
        services: [{ name: servicePkg.name }],
      }),
    ],
  };

  return [webApp, service];
};
