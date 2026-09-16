/*
 * uart_manage.c - Zephyr port (see uart_manage.h for the design notes).
 */
#include "uart_manage.h"

#include <string.h>
#include <zephyr/kernel.h>

static uart_inferface_t uart_manage[UART_MANAGE_MAX_OBJECTS] = {0};

static int uart_manage_find_slot_by_dev(const struct device *dev)
{
  if (dev == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  for (uint32_t i = 0U; i < UART_MANAGE_MAX_OBJECTS; ++i)
  {
    if ((uart_manage[i].used != 0U) && (uart_manage[i].uart_dev == dev))
    {
      return (int)i;
    }
  }

  return UART_MANAGE_ERR_NOT_FOUND;
}

static int uart_manage_find_free_slot(void)
{
  for (uint32_t i = 0U; i < UART_MANAGE_MAX_OBJECTS; ++i)
  {
    if (uart_manage[i].used == 0U)
    {
      return (int)i;
    }
  }

  return UART_MANAGE_ERR_NO_SLOT;
}

uart_inferface_t *uart_manage_get_obj(const struct device *dev)
{
  if (dev == NULL)
  {
    return NULL;
  }

  for (uint32_t i = 0U; i < UART_MANAGE_MAX_OBJECTS; ++i)
  {
    if ((uart_manage[i].used != 0U) && (uart_manage[i].uart_dev == dev))
    {
      return &uart_manage[i];
    }
  }

  return NULL;
}

uart_inferface_t *uart_manage_get_obj_by_name(const char *name)
{
  if (name == NULL)
  {
    return NULL;
  }

  for (uint32_t i = 0U; i < UART_MANAGE_MAX_OBJECTS; ++i)
  {
    if ((uart_manage[i].used != 0U) && (strcmp(uart_manage[i].name, name) == 0))
    {
      return &uart_manage[i];
    }
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/* TX pump: called from the UART ISR while is_sending is active.       */
/* ------------------------------------------------------------------ */
static void uart_manage_tx_pump(uart_inferface_t *m_obj)
{
  const struct device *dev;

  if (m_obj == NULL)
  {
    return;
  }

  dev = m_obj->uart_dev;

  if (m_obj->send_idx >= m_obj->send_len)
  {
    /* Current buffer chunk is done - pull the next one from the FIFO. */
    if (m_obj->send_fifo.used_num > 0)
    {
      uint16_t n = (uint16_t)m_obj->send_fifo.used_num;
      if (n > m_obj->send_buffer_size)
      {
        n = m_obj->send_buffer_size;
      }
      if (fifo_s_gets(&m_obj->send_fifo, (char *)m_obj->send_buffer, n) == n)
      {
        m_obj->send_idx = 0U;
        m_obj->send_len = n;
      }
      else
      {
        m_obj->send_idx = 0U;
        m_obj->send_len = 0U;
      }
    }
    else
    {
      /* All data sent. */
      m_obj->is_sending = 0U;
      m_obj->send_idx = 0U;
      m_obj->send_len = 0U;
      uart_irq_tx_disable(dev);
      (void)uart_manage_send_completed_hook(m_obj);
      return;
    }
  }

  if (m_obj->send_idx < m_obj->send_len)
  {
    uart_irq_tx_enable(dev);
    while (m_obj->send_idx < m_obj->send_len && uart_irq_tx_ready(dev))
    {
      int written = uart_fifo_fill(dev,
                                   &m_obj->send_buffer[m_obj->send_idx],
                                   m_obj->send_len - m_obj->send_idx);
      if (written <= 0)
      {
        break;
      }
      m_obj->send_idx += (uint16_t)written;
    }
  }
}

/* ------------------------------------------------------------------ */
/* RX idle timer: frame delimiter (3.5 char times) expired.           */
/* ------------------------------------------------------------------ */
static void uart_manage_rx_idle_handler(struct k_timer *timer)
{
  uart_inferface_t *m_obj = CONTAINER_OF(timer, uart_inferface_t, rx_idle_timer);
  uint8_t *frame;
  uint16_t size;
  unsigned int key;

  if (m_obj->rx_idx > 0U)
  {
    /*
     * Copy the frame out of the shared receive buffer with interrupts
     * masked: the UART ISR (higher priority) would otherwise start storing
     * the next bytes at recv_buffer[0] and overwrite the head of this frame
     * while the application copies it.
     */
    key = irq_lock();

    size = (m_obj->rx_idx <= m_obj->recv_buffer_size) ?
               m_obj->rx_idx :
               m_obj->recv_buffer_size;
    m_obj->rx_idx = 0U;

    frame = m_obj->recv_buffer;
    if (m_obj->recv_buffer_alt != NULL)
    {
      (void)memcpy(m_obj->recv_buffer_alt, m_obj->recv_buffer, size);
      frame = m_obj->recv_buffer_alt;
    }

    irq_unlock(key);

    if (m_obj->recv_callback != NULL)
    {
      (void)m_obj->recv_callback(frame, size);
    }
    else
    {
      (void)uart_manage_write_to_recv_ring(m_obj, frame, size);
    }
  }
}

/* ------------------------------------------------------------------ */
/* UART interrupt handler (RX + TX).                                   */
/* ------------------------------------------------------------------ */
static void uart_manage_irq_handler(const struct device *dev, void *user_data)
{
  uart_inferface_t *m_obj = (uart_inferface_t *)user_data;

  if (m_obj == NULL)
  {
    return;
  }

  uart_irq_update(dev);

  if (uart_irq_rx_ready(dev))
  {
    uint8_t byte;

    while (uart_fifo_read(dev, &byte, 1U) == 1)
    {
      if (m_obj->rx_idx < m_obj->recv_buffer_size)
      {
        m_obj->recv_buffer[m_obj->rx_idx++] = byte;
      }
      /* restart the idle timer for each byte */
      k_timer_start(&m_obj->rx_idle_timer, K_MSEC(m_obj->rx_idle_ms), K_NO_WAIT);
    }
  }

  if (m_obj->is_sending != 0U && uart_irq_tx_ready(dev))
  {
    uart_manage_tx_pump(m_obj);
  }
}

int uart_manage_register_interface(uart_inferface_t *m_obj)
{
  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->uart_dev == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (((m_obj->send_fifo_buffer == NULL) && (m_obj->send_fifo_size > 0U)) ||
      ((m_obj->process_buffer == NULL) && (m_obj->process_buffer_size > 0U)))
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (((m_obj->send_fifo_buffer != NULL) && (m_obj->send_fifo_size == 0U)) ||
      ((m_obj->process_buffer != NULL) && (m_obj->process_buffer_size == 0U)))
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  int slot = uart_manage_find_slot_by_dev(m_obj->uart_dev);
  if (slot < 0)
  {
    slot = uart_manage_find_free_slot();
  }

  if (slot < 0)
  {
    return slot;
  }

  uart_manage[slot] = *m_obj;
  uart_manage[slot].used = 1U;
  uart_manage[slot].idx = (uint8_t)slot;

  if (uart_manage[slot].send_fifo_buffer != NULL && uart_manage[slot].send_fifo_size > 0U)
  {
    if (fifo_s_init(&uart_manage[slot].send_fifo, uart_manage[slot].send_fifo_buffer, uart_manage[slot].send_fifo_size) != 0)
    {
      memset(&uart_manage[slot], 0, sizeof(uart_manage[slot]));
      return UART_MANAGE_ERR_FIFO;
    }
  }
  uart_manage[slot].is_sending = 0U;
  uart_manage[slot].send_idx = 0U;
  uart_manage[slot].send_len = 0U;

  if ((uart_manage[slot].process_buffer != NULL) && (uart_manage[slot].process_buffer_size > 0U))
  {
    if (lwrb_init(&uart_manage[slot].process_ring_buffer, uart_manage[slot].process_buffer, uart_manage[slot].process_buffer_size) == 0U)
    {
      memset(&uart_manage[slot], 0, sizeof(uart_manage[slot]));
      return UART_MANAGE_ERR_RING;
    }
  }

  /* idle timer = 3.5 char times, minimum 2 ms */
  if (uart_manage[slot].baudrate > 0U)
  {
    uint32_t idle = (35000UL / uart_manage[slot].baudrate) + 1U;
    uart_manage[slot].rx_idle_ms = (uint8_t)((idle < 2U) ? 2U : idle);
  }
  else
  {
    uart_manage[slot].rx_idle_ms = 5U;
  }
  k_timer_init(&uart_manage[slot].rx_idle_timer, uart_manage_rx_idle_handler, NULL);
  k_timer_user_data_set(&uart_manage[slot].rx_idle_timer, &uart_manage[slot]);

  return UART_MANAGE_OK;
}

int uart_manage_set_recv_callback(const struct device *dev, interface_recv_fn_t recv_callback)
{
  if (dev == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  uart_inferface_t *obj = uart_manage_get_obj(dev);
  if (obj == NULL)
  {
    return UART_MANAGE_ERR_NOT_FOUND;
  }

  obj->recv_callback = recv_callback;
  return UART_MANAGE_OK;
}

int uart_manage_enable_recv(const struct device *dev)
{
  if (dev == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  uart_inferface_t *m_obj = uart_manage_get_obj(dev);

  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_NOT_FOUND;
  }

  if (m_obj->uart_dev == NULL || m_obj->recv_buffer == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->recv_buffer_size == 0U)
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  m_obj->rx_idx = 0U;
  uart_irq_callback_user_data_set(dev, uart_manage_irq_handler, m_obj);
  uart_irq_rx_enable(dev);

  return UART_MANAGE_OK;
}

int uart_manage_enable_recv_by_name(const char *name)
{
  if (name == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  uart_inferface_t *m_obj = uart_manage_get_obj_by_name(name);

  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_NOT_FOUND;
  }

  return uart_manage_enable_recv(m_obj->uart_dev);
}

static int uart_manage_send_impl(uart_inferface_t *m_obj, const uint8_t *buf, uint16_t len)
{
  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_NOT_FOUND;
  }

  if (buf == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (len == 0U)
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  if ((m_obj->uart_dev == NULL) || (m_obj->send_buffer == NULL))
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->send_buffer_size == 0U)
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  uint16_t to_send_len = 0U;
  uint16_t to_tx_fifo_len = 0U;

  if (m_obj->is_sending == 0U)
  {
    uint32_t max_len = (uint32_t)m_obj->send_buffer_size + (uint32_t)m_obj->send_fifo_size;
    if ((uint32_t)len > max_len)
    {
      len = (uint16_t)max_len;
    }

    if (len <= m_obj->send_buffer_size)
    {
      to_send_len = len;
      to_tx_fifo_len = 0;
    }
    else
    {
      to_send_len = m_obj->send_buffer_size;
      to_tx_fifo_len = (uint16_t)(len - m_obj->send_buffer_size);
    }
  }
  else
  {
    if (len > m_obj->send_fifo_size)
    {
      len = m_obj->send_fifo_size;
    }

    if (len > 0U)
    {
      to_send_len = 0;
      to_tx_fifo_len = len;
    }
  }

  if (len == 0U)
  {
    return UART_MANAGE_ERR_NO_SPACE;
  }

  if ((to_tx_fifo_len > 0U) && (m_obj->send_fifo_buffer == NULL))
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (to_send_len > 0)
  {
    memcpy(m_obj->send_buffer, buf, to_send_len);
    m_obj->send_idx = 0U;
    m_obj->send_len = to_send_len;
    m_obj->is_sending = 1U;
    uart_manage_tx_pump(m_obj);
  }

  if (to_tx_fifo_len > 0)
  {
    int put_len;
    put_len = fifo_s_puts(&m_obj->send_fifo, (char *)(buf) + to_send_len, to_tx_fifo_len);
    if (put_len != (int)to_tx_fifo_len)
    {
      return UART_MANAGE_ERR_FIFO;
    }
  }

  return UART_MANAGE_OK;
}

int uart_manage_send(const struct device *dev, const uint8_t *buf, uint16_t len)
{
  if (dev == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  uart_inferface_t *m_obj = uart_manage_get_obj(dev);
  return uart_manage_send_impl(m_obj, buf, len);
}

int uart_manage_send_by_name(const char *name, const uint8_t *buf, uint16_t len)
{
  if (name == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  uart_inferface_t *m_obj = uart_manage_get_obj_by_name(name);
  return uart_manage_send_impl(m_obj, buf, len);
}

int uart_manage_send_completed_hook(uart_inferface_t *m_obj)
{
  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->send_fifo.used_num > 0)
  {
    /* Keep the TX IRQ running; the next chunk is handled by the pump. */
    uart_irq_tx_enable(m_obj->uart_dev);
  }

  return UART_MANAGE_OK;
}

int uart_manage_reset_send(uart_inferface_t *m_obj)
{
  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  m_obj->is_sending = 0U;
  m_obj->send_idx = 0U;
  m_obj->send_len = 0U;
  if (m_obj->uart_dev != NULL)
  {
    uart_irq_tx_disable(m_obj->uart_dev);
  }
  return UART_MANAGE_OK;
}

int uart_manage_write_to_recv_ring(uart_inferface_t *m_obj, uint8_t *buf, uint16_t len)
{
  if (m_obj == NULL || buf == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (len == 0U)
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  if (m_obj->process_buffer == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->process_buffer_size == 0U)
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  lwrb_sz_t to_write_len = (lwrb_sz_t)len;
  lwrb_sz_t free_len = lwrb_get_free(&m_obj->process_ring_buffer);
  if (to_write_len > free_len)
  {
    to_write_len = free_len;
  }

  if (to_write_len == 0U)
  {
    return UART_MANAGE_ERR_NO_SPACE;
  }

  if ((to_write_len > 0U) && (lwrb_write(&m_obj->process_ring_buffer, buf, to_write_len) != to_write_len))
  {
    return UART_MANAGE_ERR_RING;
  }

  return (int)to_write_len;
}

int uart_manage_recv_idle_hook(uart_inferface_t *m_obj, interrput_type int_type, uint16_t size)
{
  if (m_obj == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if (m_obj->recv_buffer == NULL)
  {
    return UART_MANAGE_ERR_POINTER_NULL;
  }

  if ((size == 0U) || (size > m_obj->recv_buffer_size))
  {
    return UART_MANAGE_ERR_INVALID_PARAM;
  }

  (void)int_type;

  if (m_obj->recv_callback != NULL)
  {
    return m_obj->recv_callback(m_obj->recv_buffer, size);
  }

  return uart_manage_write_to_recv_ring(m_obj, m_obj->recv_buffer, size);
}
