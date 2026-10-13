# grow-fullstack-iot




<div align="center">

# 🌱 Grow Monitor

### Sistema IoT completo para monitoramento e controle de grow

[![ESP32](https://img.shields.io/badge/ESP32-2x-E7352C?style=for-the-badge&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Azure](https://img.shields.io/badge/Azure-IoT%20Hub-0078D4?style=for-the-badge&logo=microsoftazure&logoColor=white)](https://azure.microsoft.com/)
[![Spring Boot](https://img.shields.io/badge/Spring%20Boot-3.x-6DB33F?style=for-the-badge&logo=springboot&logoColor=white)](https://spring.io/projects/spring-boot)
[![Angular](https://img.shields.io/badge/Angular-17+-DD0031?style=for-the-badge&logo=angular&logoColor=white)](https://angular.io/)
[![License](https://img.shields.io/badge/License-MIT-blue?style=for-the-badge)](LICENSE)

</div>

---

## 📖 Sobre o Projeto

O **Grow Monitor** é um sistema completo de **monitoramento e controle automatizado de grow**, composto por:

- 🌡️ **Sensores físicos** (temperatura, umidade do ar, umidade do solo, nível d'água)
- 💡 **Controle de atuadores** (luz, bomba d'água, ventilador, exaustor, torneira)
- ☁️ **Comunicação segura** com a nuvem via **Azure IoT Hub (TLS + autenticação)**
- 🖥️ **Backend** em Spring Boot que consome os dados da nuvem
- 🌐 **Frontend** em Angular com dashboard em tempo real

O sistema foi projetado para operar de forma **autônoma** (envio periódico de telemetria), **sob demanda** (comandos C2D) e **segura** (criptografia de ponta a ponta).

---

## 🏗️ Arquitetura
---

<img width="2790" height="6965" alt="ESP32 Telemetry and C2D Pipeline-2026-10-13-121655" src="https://github.com/user-attachments/assets/974274a5-68f4-467d-9de6-6650337f1b8c" />


---

## 📂 Estrutura do Repositório


<img width="5760" height="5495" alt="ESP32 Telemetry and C2D Pipeline-2026-10-13-121858" src="https://github.com/user-attachments/assets/38c2e16e-3c09-420c-b716-296b53debebd" />


---

## ✨ Funcionalidades

### 🌡️ Monitoramento
- [x] Temperatura do ar (AHT30)
- [x] Umidade do ar (AHT30)
- [x] Umidade do solo (sensor capacitivo)
- [x] Nível d'água (2 sensores de boia)
- [x] Status de todos os relés em tempo real

### 🎛️ Controle Automático
- [x] Ciclo de luz configurável (18/6 ou 12/12)
- [x] Irrigação automática por umidade do solo
- [x] Controle de temperatura e umidade (ventilador + exaustor)
- [x] Enchimento automático do reservatório

### ☁️ Cloud & Segurança
- [x] Comunicação MQTT sobre TLS com Azure IoT Hub
- [x] Autenticação por SAS Token (renovação automática)
- [x] Envio periódico de telemetria (1 hora)
- [x] Envio sob demanda via comandos C2D
- [x] Dados criptografados de ponta a ponta

### 🖥️ Backend & Frontend
- [x] API REST em Spring Boot
- [x] Consumer do Event Hub-compatible endpoint
- [x] Dashboard Angular com atualização em tempo real
- [x] Interface responsiva (desktop e mobile)

---

## 🚀 Começando

### Pré-requisitos

| Componente | Versão mínima |
|---|---|
| Arduino IDE | 2.0+ |
| ESP32 Board Package | 3.0+ |
| Java | 17+ |
| Maven | 3.8+ |
| Node.js | 20+ |
| Angular CLI | 17+ |
| Conta Azure | (gratuita) |

### 1️⃣ Configurar o Azure IoT Hub

```bash
# Criar um IoT Hub (camada gratuita)
az iot hub create --name grow-monitor-hub --resource-group grow-rg --sku F1

# Registrar o dispositivo gateway
az iot hub device-identity create --hub-name grow-monitor-hub --device-id esp32-gateway

# Obter a chave primária
az iot hub device-identity show --hub-name grow-monitor-hub --device-id esp32-gateway
📖 Detalhes completos em docs/setup-azure.md

2️⃣ Gravar os ESP32
Emissor (lê sensores e envia JSON via UART):

bash
# Abra firmware/esp32-emissor/esp32-emissor.ino no Arduino IDE
# Selecione a placa ESP32 Dev Module
# Grave normalmente
Gateway (recebe JSON e publica no Azure):

bash
# 1. Edite firmware/esp32-gateway/iot_configs.h com suas credenciais
# 2. Abra firmware/esp32-gateway/esp32-gateway.ino
# 3. Grave normalmente
📖 Detalhes completos em docs/setup-esp32.md

3️⃣ Rodar o Backend Spring Boot
bash
cd backend/spring
./mvnw spring-boot:run
A API estará disponível em http://localhost:8080

📖 Detalhes completos em docs/setup-spring.md

4️⃣ Rodar o Frontend Angular
bash
cd frontend/angular
npm install
ng serve
Acesse http://localhost:4200

🔌 Hardware
ESP32 Emissor
Componente	GPIO	Descrição
AHT30 (SDA)	21	Sensor de temperatura/umidade
AHT30 (SCL)	22	Sensor de temperatura/umidade
Sensor nível alto	34	Boia superior
Sensor nível baixo	39	Boia inferior
Sensor solo	35	Umidade do solo
Relé bomba	14	Bomba d'água
Relé ventilador	27	Ventilador
Relé exaustor	33	Exaustor
Relé luz	32	Iluminação
Torneira fechar	26	Válvula fechar
Torneira abrir	25	Válvula abrir
TX UART	17	Envia JSON para o gateway
ESP32 Gateway
Componente	GPIO	Descrição
RX UART	16	Recebe JSON do emissor
Wi-Fi	—	Conexão com Azure IoT Hub
⚠️ Importante: os GNDs dos dois ESP32 devem estar conectados entre si.

🔐 Segurança
Camada	Mecanismo
Transporte	MQTT sobre TLS 1.2+
Autenticação	SAS Token (renovação automática a cada 60 min)
Criptografia	Certificado CA da Azure validado no ESP32
Isolamento	ESP32 nunca exposto à internet (só Azure)
Fila C2D	Comandos assinados e autenticados pelo IoT Hub

📊 Fluxo de Dados
ESP32 Emissor lê sensores a cada 1s e monta um JSON

Envia o JSON via UART (115200 baud) para o ESP32 Gateway

O Gateway acumula o JSON até encontrar \n

Decide enviar quando:

⏰ Passou 1 hora desde o último envio, ou

🔄 O JSON foi atualizado e passou 5 minutos do último envio, ou

📩 O Spring enviou o comando C2D SOLICITAR_STATUS

Publica no Azure IoT Hub via MQTT sobre TLS

O Spring Boot consome do Event Hub-compatible endpoint

O Angular chama a API REST do Spring e exibe no dashboard

🧪 Tecnologias Utilizadas
Firmware
Arduino C++ para ESP32

Azure SDK for C (azure-sdk-for-c-arduino)

PubSubClient (indireto via mqtt_client.h do ESP-IDF)

Backend
Spring Boot 3.x

Azure Messaging SDK (Event Hub consumer)

Spring Data JPA (persistência)

PostgreSQL (banco de dados)

Frontend
Angular 17+

TypeScript

RxJS

Chart.js (gráficos)

Cloud
Azure IoT Hub (comunicação segura)

Azure Event Hub-compatible endpoint (consumo)
