FROM node:20-alpine
WORKDIR /app
RUN apk add --no-cache curl
COPY package*.json ./
RUN npm ci --only=production
COPY src ./src
RUN mkdir -p /storage/recordings /storage/hls
EXPOSE 3001
HEALTHCHECK --interval=5s --timeout=5s --retries=15 CMD curl -sf http://localhost:3001/api/health || exit 1
CMD ["node", "src/index.js"]
