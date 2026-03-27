#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include "AppInsights.h"
#include <Stepper.h> 
#include "time.h"

// --- DISPLAY LCD 16x2 I2C ---
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 16, 2); 

// Prevenção de Brownout
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// --- CONFIGURAÇÕES ---
#define DEFAULT_POWER_MODE false 
const char *service_name = "PROV_alimentador";
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

int qtd_porcoes = 1; 
char texto_ultima_refeicao[10] = "--:--";

// --- CONTROLES E FLAGS ---
unsigned long momento_liberacao = 0; 
unsigned long ultimoUpdate = 0;
#define TEMPO_COOLDOWN 1000 
bool dnd_ativo = false;    
float dnd_inicio = 22.0;   
float dnd_fim = 7.0;   
bool tela_ligada = true;
bool sincronizar_tela_app = false;
bool display_conectado = false;

// Flag crucial para tirar o peso do callback do RainMaker
volatile int porcoes_pendentes = 0; 

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

static Device *my_switch = NULL;

void getHoraAtual(char *buffer, size_t tamanho) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 0)) {
        strncpy(buffer, "--:--", tamanho);
        return;
    }
    strftime(buffer, tamanho, "%H:%M", &timeinfo);
}

// --- FUNÇÃO DE DESENHO (LCD) ---
void atualizarTela(String titulo, String rodape = "", bool limpar = true) {
    if (!display_conectado) return;
    if (!sessao_provisionamento_encerrada && titulo != "SETUP") return;
    if (!tela_ligada) return;

    if (limpar) lcd.clear();

    if (titulo == "ONLINE") {
        char horaBuff[10];
        getHoraAtual(horaBuff, sizeof(horaBuff));

        // Linha de cima: WrFEED (W) 14:30
        lcd.setCursor(0, 0);
        lcd.print("WrFEED "); 
        if (WiFi.status() == WL_CONNECTED) lcd.print("(W) ");
        else lcd.print("(!) ");
        lcd.print(horaBuff);

        // Linha de baixo: Ultima: 14:30
        lcd.setCursor(0, 1);
        lcd.print("Ultima: ");
        lcd.print(texto_ultima_refeicao);
    }
    else {
        // Telas de ação (Ex: ALIMENTANDO / Porcao 1/1)
        lcd.setCursor(0, 0);
        lcd.print(titulo);
        lcd.setCursor(0, 1);
        lcd.print(rodape);
    }
}

void mostrarTelaPareamento() {
    if (!display_conectado) return;
    lcd.clear();
    lcd.setCursor(0, 0); 
    lcd.print("MODO PAREAMENTO");
    lcd.setCursor(0, 1);
    lcd.print("App > Add > BLE");
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

            digitalWrite(gpio_switch, HIGH);
            porcoes_pendentes = qtd_porcoes;
            
            // Deixamos o botão "ligado" no app temporariamente. O loop() vai desligar quando terminar.
            param->updateAndReport(val);

        } else {
            digitalWrite(gpio_switch, LOW);
            param->updateAndReport(val);
        }
    }
    else if (strcmp(param_name, "Tela LCD") == 0) {
        tela_ligada = val.val.b;
        if (display_conectado) {
            if (tela_ligada) {
                lcd.backlight(); 
                if(sessao_provisionamento_encerrada) {
                    char horaTemp[10];
                    getHoraAtual(horaTemp, sizeof(horaTemp));
                    atualizarTela("ONLINE", horaTemp);
                }
            } else {
                lcd.clear();
                lcd.noBacklight(); 
            }
        }
        param->updateAndReport(val);
    }
    else if (strcmp(param_name, "Qtd Porcoes") == 0) {
        qtd_porcoes = val.val.i;
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

    Wire.begin();
    Wire.beginTransmission(0x27); 
    if (Wire.endTransmission() == 0) {
        lcd.init();
        lcd.backlight();
        display_conectado = true;
        mostrarTelaPareamento(); 
    } else {
        Serial.println(F("Erro LCD - Display I2C nao encontrado. Pulando interface visual."));
        display_conectado = false;
    }

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

  // Cria o dispositivo genérico, mas avisa o App que ele se comporta como um Switch (para manter o ícone)
    my_switch = new Device("Alimentador", ESP_RMAKER_DEVICE_SWITCH);
    if (!my_switch) return;

    // 1º DA LISTA: Quantidade de Porções (Será lido antes de ligar o motor)
    Param qtd("Qtd Porcoes", ESP_RMAKER_PARAM_RANGE, value(qtd_porcoes), PROP_FLAG_READ | PROP_FLAG_WRITE);
    qtd.addBounds(value(1), value(5), value(1)); 
    qtd.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(qtd);

    // 2º DA LISTA: Botão de Power (Será lido com a Qtd Porções já atualizada na memória)
    Param power("Power", ESP_RMAKER_PARAM_POWER, value(DEFAULT_POWER_MODE), PROP_FLAG_READ | PROP_FLAG_WRITE);
    power.addUIType(ESP_RMAKER_UI_TOGGLE);
    my_switch->addParam(power);
    if (!my_switch) return;
    
    my_switch->addParam(Param("Ultima Refeicao", "esp.param.text", value("---"), PROP_FLAG_READ));

    Param tela_sw("Tela LCD", ESP_RMAKER_PARAM_POWER, value(tela_ligada), PROP_FLAG_READ | PROP_FLAG_WRITE);
    tela_sw.addUIType(ESP_RMAKER_UI_TOGGLE);
    my_switch->addParam(tela_sw);

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
    if (porcoes_pendentes > 0 && sessao_provisionamento_encerrada) {
        
        atualizarTela("ALIMENTANDO", "Aguarde...");
        
        for (int i = 1; i <= porcoes_pendentes; i++) {
            String status = "PORCAO " + String(i) + "/" + String(porcoes_pendentes);
            atualizarTela(status, "Girando...");
            
            meuMotor.step(PASSOS_POR_VOLTA); 

            // IMPORTANTÍSSIMO: Desliga as bobinas do ULN2003 para o motor não derreter 
            digitalWrite(PINO_IN1, LOW);
            digitalWrite(PINO_IN2, LOW);
            digitalWrite(PINO_IN3, LOW);
            digitalWrite(PINO_IN4, LOW);

            if (i < porcoes_pendentes) {
                delay(1500); 
            }
        }

        // --- FINALIZAÇÃO E ATUALIZAÇÃO DO APP ---
        getHoraAtual(texto_ultima_refeicao, sizeof(texto_ultima_refeicao));

        if (my_switch) {
             struct tm timeinfo;
             if(getLocalTime(&timeinfo, 0)){
                  char timeStringBuff[50];
                  strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m as %H:%M", &timeinfo);
                  my_switch->updateAndReportParam("Ultima Refeicao", timeStringBuff);
             }
             // Retorna o botão principal para OFF no aplicativo do celular
             my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false); 
        }

        digitalWrite(gpio_switch, LOW);
        momento_liberacao = millis() + TEMPO_COOLDOWN;
        porcoes_pendentes = 0; // Zera a fila
        
        char horaTemp[10];
        getHoraAtual(horaTemp, sizeof(horaTemp));
        atualizarTela("ONLINE", horaTemp);
    }

    if (sessao_provisionamento_encerrada && WiFi.status() == WL_CONNECTED) {
        
        if (sincronizar_tela_app) {
            if (my_switch) my_switch->updateAndReportParam("Tela LCD", tela_ligada);
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
                        if (display_conectado) {
                            lcd.clear();
                            lcd.noBacklight();
                        }
                    } else { 
                        tela_ligada = true;
                        if (display_conectado) {
                            lcd.backlight();
                        }
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
