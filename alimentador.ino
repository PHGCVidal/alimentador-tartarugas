#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include "AppInsights.h"
#include <Stepper.h> // <-- Substituído ESP32Servo por Stepper
#include "time.h"

// --- DISPLAY ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Prevenção de Brownout
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- CONFIGURAÇÕES ---
#define DEFAULT_POWER_MODE false 
const char *service_name = "PROV_aromatizador";
const char *pop = "123456"; 

// --- MECÂNICA (MOTOR DE PASSO 28BYJ-48) ---
// O 28BYJ-48 tem 2048 passos por volta completa no modo de 4 passos
#define PASSOS_POR_VOLTA 2048 
#define PINO_IN1 25
#define PINO_IN2 26
#define PINO_IN3 27
#define PINO_IN4 14

// Inicializa o motor. IMPORTANTE: A sequência IN1, IN3, IN2, IN4 é necessária para o ULN2003
Stepper meuMotor(PASSOS_POR_VOLTA, PINO_IN1, PINO_IN3, PINO_IN2, PINO_IN4);

int qtd_disparos = 1; 
char texto_ultimo_disparo[10] = "--:--";

// --- CONTROLES E FLAGS ---
unsigned long momento_liberacao = 0; 
unsigned long ultimoUpdate = 0;
#define TEMPO_COOLDOWN 1000 
bool dnd_ativo = false;    
float dnd_inicio = 22.0;   
float dnd_fim = 7.0;   
bool tela_ligada = true;
bool sincronizar_tela_app = false;

// Flag crucial para tirar o peso do callback do RainMaker
volatile int disparos_pendentes = 0; 

// Trava de segurança do Display
bool sessao_provisionamento_encerrada = false; 
unsigned long timestamp_fim_prov = 0;

// GPIO
#if CONFIG_IDF_TARGET_ESP32C3
static int gpio_0 = 9;
static int gpio_switch = 7;
static int gpio_reset = 4;
#else
static int gpio_0 = 0;
static int gpio_switch = 2;
static int gpio_reset = 33;
#endif

static Switch *my_switch = NULL;

void getHoraAtual(char *buffer, size_t tamanho) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 0)) {
        strncpy(buffer, "--:--", tamanho);
        return;
    }
    strftime(buffer, tamanho, "%H:%M", &timeinfo);
}

// --- FUNÇÃO DE DESENHO ---
void atualizarTela(String titulo, String rodape = "", bool limpar = true) {
    if (!sessao_provisionamento_encerrada && titulo != "SETUP") return;
    if (!tela_ligada) return;

    if (limpar) display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("WrAIR")); 
    display.setCursor(110, 0); 
    if (WiFi.status() == WL_CONNECTED) display.print(F("(W)"));
    else display.print(F("(!)"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    if (titulo == "ONLINE") {
        char horaBuff[10];
        getHoraAtual(horaBuff, sizeof(horaBuff));

        display.setTextSize(3);
        int len = strlen(horaBuff);
        int x_hora = (128 - (len * 18)) / 2;
        if(x_hora < 0) x_hora = 0;
        
        display.setCursor(x_hora, 22);
        display.print(horaBuff);

        display.setTextSize(1);
        display.setCursor(0, 54);
        display.print(F("Ultimo: "));
        display.print(texto_ultimo_disparo);
    }
    else {
        display.setTextSize(2);
        int x_titulo = (128 - (titulo.length() * 12)) / 2;
        if(x_titulo < 0) x_titulo = 0;

        display.setCursor(x_titulo, 25);
        display.print(titulo);

        display.setTextSize(1);
        display.setCursor(0, 54);
        display.print(rodape);
    }

    display.display();
}

void mostrarTelaPareamento() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    display.setTextSize(1);
    display.setCursor(0, 0); 
    display.println("MODO PAREAMENTO");
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    display.setCursor(0, 20);
    display.println("Abra App RainMaker");
    display.setCursor(0, 32);
    display.println("No QRCode > BLE'");

    display.setTextSize(2);
    display.setCursor(0, 48);
    display.print("POP:"); display.println(pop); 

    display.display();
}

void sysProvEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
#if CONFIG_IDF_TARGET_ESP32S2
        Serial.printf("\nSoftAP Provisioning\n");
        printQR(service_name, pop, "softap");
#else
        Serial.printf("\nBLE Provisioning\n");
        printQR(service_name, pop, "ble");
#endif
        break;
        
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
        Serial.println("Credenciais recebidas! Conectando...");
        break;

    case ARDUINO_EVENT_PROV_END:
        Serial.println("\n--- FIM DO PROVISIONAMENTO ---");
        sessao_provisionamento_encerrada = true;
        timestamp_fim_prov = millis();
        break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.println("IP Obtido! Conectado ao Wi-Fi.");
        sessao_provisionamento_encerrada = true;
        break;
        
    default:;
    }
}

bool verificarPodeDisparar() {
    if (!dnd_ativo) return true; 
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 0)) return true; 

    int minutos_agora = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
    int minutos_inicio = dnd_inicio * 60; 
    int minutos_fim = dnd_fim * 60;       

    bool dentro = false;
    if (minutos_inicio < minutos_fim) {
        if (minutos_agora >= minutos_inicio && minutos_agora < minutos_fim) dentro = true;
    } else {
        if (minutos_agora >= minutos_inicio || minutos_agora < minutos_fim) dentro = true;
    }

    if (dentro) {
        if (sessao_provisionamento_encerrada) { 
            atualizarTela("BLOQUEADO", "Modo DND Ativo");
            // Substituí delay(2000) por algo não bloqueante se chamado no callback, 
            // mas como é só leitura de estado, o ideal é não travar o painel.
            char horaBuff[10]; 
            getHoraAtual(horaBuff, sizeof(horaBuff)); 
            atualizarTela("ONLINE", horaBuff); 
        }
        return false; 
    }
    return true; 
}

void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx)
{
    const char *param_name = param->getParamName();

    if (strcmp(param_name, "Power") == 0) {
        if (val.val.b == true) {

            if (!verificarPodeDisparar()) {
                param->updateAndReport(value(false));
                return;
            }
            
            if (millis() < momento_liberacao) {
                if(sessao_provisionamento_encerrada) {
                    atualizarTela("COOLDOWN", "Aguarde...");
                }
                param->updateAndReport(value(false)); 
                return; 
            }

            // *** O SEGREDO ESTÁ AQUI ***
            // Em vez de executar o motor de passo e travar o RainMaker, apenas avisamos
            // o loop() que ele precisa fazer isso.
            digitalWrite(gpio_switch, HIGH);
            disparos_pendentes = qtd_disparos;
            
            // Deixamos o botão "ligado" no app temporariamente. O loop() vai desligar quando terminar.
            param->updateAndReport(val);

        } else {
            digitalWrite(gpio_switch, LOW);
            param->updateAndReport(val);
        }
    }
    else if (strcmp(param_name, "Tela OLED") == 0) {
        tela_ligada = val.val.b;
        if (tela_ligada) {
            display.ssd1306_command(SSD1306_DISPLAYON);
            if(sessao_provisionamento_encerrada) {
                char horaTemp[10];
                getHoraAtual(horaTemp, sizeof(horaTemp));
                atualizarTela("ONLINE", horaTemp);
            }
        } else {
            display.clearDisplay();
            display.display();
            display.ssd1306_command(SSD1306_DISPLAYOFF);
        }
        param->updateAndReport(val);
    }
    else if (strcmp(param_name, "Qtd Sprays") == 0) {
        qtd_disparos = val.val.i;
        param->updateAndReport(val);
    }
    else if (strcmp(param_name, "Modo Noturno") == 0) {
        dnd_ativo = val.val.b;
        ultimoUpdate = 0;
        param->updateAndReport(val);
    }
    else if (strcmp(param_name, "DND Inicio") == 0) {
        dnd_inicio = val.val.f; 
        ultimoUpdate = 0;
        param->updateAndReport(val);
    }
    else if (strcmp(param_name, "DND Fim") == 0) {
        dnd_fim = val.val.f;   
        ultimoUpdate = 0; 
        param->updateAndReport(val);
    }
}

void setup()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    configTzTime("<-03>3", "pool.ntp.org", "time.nist.gov");

    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
        Serial.println(F("Erro OLED"));
    }
    mostrarTelaPareamento(); 

    pinMode(gpio_reset, INPUT_PULLUP);
    pinMode(gpio_switch, OUTPUT);
    digitalWrite(gpio_switch, DEFAULT_POWER_MODE);

    // Configura velocidade do motor de passo (15 RPM é seguro para o 28BYJ-48 não pular passos)
    meuMotor.setSpeed(15); 

    // Define os pinos do driver do motor como saída e desliga por padrão
    pinMode(PINO_IN1, OUTPUT); pinMode(PINO_IN2, OUTPUT);
    pinMode(PINO_IN3, OUTPUT); pinMode(PINO_IN4, OUTPUT);
    digitalWrite(PINO_IN1, LOW); digitalWrite(PINO_IN2, LOW);
    digitalWrite(PINO_IN3, LOW); digitalWrite(PINO_IN4, LOW);

    Node my_node;
    my_node = RMaker.initNode("ESP RainMaker Node");

    my_switch = new Switch("Switch", &gpio_switch);
    if (!my_switch) return;
    
    my_switch->addParam(Param("Ultimo Disparo", "esp.param.text", value("---"), PROP_FLAG_READ));

    Param tela_sw("Tela OLED", ESP_RMAKER_PARAM_POWER, value(tela_ligada), PROP_FLAG_READ | PROP_FLAG_WRITE);
    tela_sw.addUIType(ESP_RMAKER_UI_TOGGLE);
    my_switch->addParam(tela_sw);

    Param qtd("Qtd Sprays", ESP_RMAKER_PARAM_RANGE, value(qtd_disparos), PROP_FLAG_READ | PROP_FLAG_WRITE);
    qtd.addBounds(value(1), value(5), value(1)); 
    qtd.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(qtd);

    Param dnd_sw("Modo Noturno", ESP_RMAKER_PARAM_POWER, value(dnd_ativo), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_sw.addUIType(ESP_RMAKER_UI_TOGGLE);
    my_switch->addParam(dnd_sw);

    Param dnd_ini("DND Inicio", ESP_RMAKER_PARAM_RANGE, value(dnd_inicio), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_ini.addBounds(value(0.0f), value(24.0f), value(0.5f)); 
    dnd_ini.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(dnd_ini);

    Param dnd_end("DND Fim", ESP_RMAKER_PARAM_RANGE, value(dnd_fim), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_end.addBounds(value(0.0f), value(24.0f), value(0.5f)); 
    dnd_end.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(dnd_end);

    my_switch->addCb(write_callback);
    my_node.addDevice(*my_switch);

    RMaker.enableOTA(OTA_USING_TOPICS);
    RMaker.enableTZService();
    RMaker.enableSchedule();
    RMaker.enableScenes();
    initAppInsights();
    RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 0, 0, 0);

    RMaker.start();

    my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);

    WiFi.onEvent(sysProvEvent);
#if CONFIG_IDF_TARGET_ESP32S2
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE, WIFI_PROV_SECURITY_1, pop, service_name);
#else
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, pop, service_name);
#endif
}

void loop()
{
    static bool status_noturno_anterior = false; 
    static bool primeira_leitura_feita = false; 

    // --- MÁQUINA DE ESTADO DO MOTOR DE PASSO ---
    // Executamos o giro aqui no loop para não engasgar o servidor web interno do RainMaker
    if (disparos_pendentes > 0 && sessao_provisionamento_encerrada) {
        
        atualizarTela("ATUANDO", "Spray...");
        
        for (int i = 1; i <= disparos_pendentes; i++) {
            String status = "SPRAY " + String(i) + "/" + String(disparos_pendentes);
            atualizarTela(status, "Girando...");
            
            // Dá UMA volta completa (2048 passos). Isso leva aprox. 4 segundos a 15 RPM.
            meuMotor.step(PASSOS_POR_VOLTA); 

            // IMPORTANTÍSSIMO: Desliga as bobinas do ULN2003 para o motor não derreter 
            // e nem puxar corrente fantasma da sua fonte.
            digitalWrite(PINO_IN1, LOW);
            digitalWrite(PINO_IN2, LOW);
            digitalWrite(PINO_IN3, LOW);
            digitalWrite(PINO_IN4, LOW);

            if (i < disparos_pendentes) {
                delay(1500); 
            }
        }

        // --- FINALIZAÇÃO E ATUALIZAÇÃO DO APP ---
        getHoraAtual(texto_ultimo_disparo, sizeof(texto_ultimo_disparo));

        if (my_switch) {
             struct tm timeinfo;
             if(getLocalTime(&timeinfo, 0)){
                  char timeStringBuff[50];
                  strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m as %H:%M", &timeinfo);
                  my_switch->updateAndReportParam("Ultimo Disparo", timeStringBuff);
             }
             // Retorna o botão principal para OFF no aplicativo do celular
             my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false); 
        }

        digitalWrite(gpio_switch, LOW);
        momento_liberacao = millis() + TEMPO_COOLDOWN;
        disparos_pendentes = 0; // Zera a fila
        
        char horaTemp[10];
        getHoraAtual(horaTemp, sizeof(horaTemp));
        atualizarTela("ONLINE", horaTemp);
    }

    if (sessao_provisionamento_encerrada && WiFi.status() == WL_CONNECTED) {
        
        if (sincronizar_tela_app) {
            if (my_switch) my_switch->updateAndReportParam("Tela OLED", tela_ligada);
            sincronizar_tela_app = false;
        }

        if (timestamp_fim_prov != 0) {
            if (millis() - timestamp_fim_prov < 3000) {
                return; 
            } else {
                timestamp_fim_prov = 0; 
                ultimoUpdate = 0; 
            }
        }

        long intervalo = 60000; 
        
        if (ultimoUpdate == 0 || millis() - ultimoUpdate > intervalo) { 
            struct tm timeinfo;
            bool hora_valida = getLocalTime(&timeinfo, 0); 
            
            if(!hora_valida){
                intervalo = 2000; 
            } else {
                intervalo = 60000;
                bool deve_estar_desligada = false;
                
                if (dnd_ativo) {
                    int minutos_agora = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
                    int minutos_inicio = dnd_inicio * 60;
                    int minutos_fim = dnd_fim * 60;

                    if (minutos_inicio < minutos_fim) {
                        if (minutos_agora >= minutos_inicio && minutos_agora < minutos_fim) deve_estar_desligada = true;
                    } else {
                        if (minutos_agora >= minutos_inicio || minutos_agora < minutos_fim) deve_estar_desligada = true;
                    }
                }

                if (deve_estar_desligada != status_noturno_anterior || !primeira_leitura_feita) {
                    status_noturno_anterior = deve_estar_desligada; 
                    
                    if (deve_estar_desligada) { 
                        tela_ligada = false;
                        display.clearDisplay();
                        display.display();
                        display.ssd1306_command(SSD1306_DISPLAYOFF);
                    } else { 
                        tela_ligada = true;
                        display.ssd1306_command(SSD1306_DISPLAYON);
                        
                        char horaTemp[10];
                        getHoraAtual(horaTemp, sizeof(horaTemp));
                        atualizarTela("ONLINE", horaTemp);
                    }
                    
                    if (primeira_leitura_feita) {
                        sincronizar_tela_app = true; 
                    }
                    primeira_leitura_feita = true;
                }
            }

            atualizarTela("ONLINE");
            ultimoUpdate = millis();
        }
    }
    
    if (digitalRead(gpio_reset) == LOW) {
        delay(200); 
        if (digitalRead(gpio_reset) == LOW) { 
            RMakerWiFiReset(2); 
            while(true) delay(100); 
        }
    }
    
    delay(50); 
}