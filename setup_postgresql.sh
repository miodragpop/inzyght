#!/bin/bash
# PostgreSQL Server Setup for Inzyght

set -e

echo "========================================="
echo "PostgreSQL Server Setup for Inzyght"
echo "========================================="
echo ""

# Check if running as root
if [ "$EUID" -eq 0 ]; then
   echo "ERROR: Please run this script as your regular user (not root)"
   echo "The script will use 'sudo' when needed"
   exit 1
fi

# Detect PostgreSQL server installation
PG_VERSION=$(pg_config --version 2>/dev/null | grep -oP '\d+' | head -1)

if [ -z "$PG_VERSION" ]; then
    echo "ERROR: PostgreSQL client tools not found"
    echo "Please install: sudo apt-get install postgresql-client"
    exit 1
fi

echo "Detected PostgreSQL version: $PG_VERSION"
echo ""

# Check if PostgreSQL server is installed
if ! dpkg -l | grep -q "postgresql-$PG_VERSION"; then
    echo "PostgreSQL server not installed. Installing..."
    sudo apt-get update
    sudo apt-get install -y postgresql-$PG_VERSION postgresql-contrib
    echo "✓ PostgreSQL server installed"
else
    echo "✓ PostgreSQL server already installed"
fi

echo ""
echo "Starting PostgreSQL service..."
sudo systemctl start postgresql
sudo systemctl enable postgresql
sleep 2

# Check if server is running
if sudo systemctl is-active --quiet postgresql; then
    echo "✓ PostgreSQL server is running"
else
    echo "ERROR: PostgreSQL server failed to start"
    echo "Check logs: sudo journalctl -xeu postgresql"
    exit 1
fi

echo ""
echo "========================================="
echo "Creating Database and User"
echo "========================================="
echo ""

# Database configuration — override via environment.
# DB_PASS must be set explicitly; no insecure default.
DB_NAME="${DB_NAME:-inzyght}"
DB_USER="${DB_USER:-ycash_user}"
DB_PASS="${DB_PASS?DB_PASS env var required: e.g. DB_PASS=mypassword ./setup_postgresql.sh}"

echo "Database: $DB_NAME"
echo "User: $DB_USER"
echo "Password: $DB_PASS"
echo ""

# Check if database exists
if sudo -u postgres psql -lqt | cut -d \| -f 1 | grep -qw "$DB_NAME"; then
    echo "⚠ Database '$DB_NAME' already exists"
    read -p "Drop and recreate? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        sudo -u postgres psql -c "DROP DATABASE IF EXISTS $DB_NAME;"
        sudo -u postgres psql -c "DROP USER IF EXISTS $DB_USER;"
        echo "✓ Dropped existing database and user"
    else
        echo "Using existing database"
        exit 0
    fi
fi

# Create user
echo "Creating PostgreSQL user..."
sudo -u postgres psql -c "CREATE USER $DB_USER WITH PASSWORD '$DB_PASS';" 2>/dev/null || echo "User already exists"

# Create database
echo "Creating database..."
sudo -u postgres psql -c "CREATE DATABASE $DB_NAME OWNER $DB_USER;"

# Grant privileges
echo "Granting privileges..."
sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE $DB_NAME TO $DB_USER;"

echo "✓ Database and user created"
echo ""

# Apply schema
echo "========================================="
echo "Applying Database Schema"
echo "========================================="
echo ""

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if [ -f "$SCRIPT_DIR/schema.sql" ]; then
    echo "Applying schema.sql..."
    PGPASSWORD="$DB_PASS" psql -U $DB_USER -d $DB_NAME -h 127.0.0.1 -f "$SCRIPT_DIR/schema.sql"
    echo "✓ schema.sql applied"
else
    echo "⚠ schema.sql not found - skipping"
fi

if [ -f "$SCRIPT_DIR/schema_reorg_support.sql" ]; then
    echo "Applying schema_reorg_support.sql..."
    PGPASSWORD="$DB_PASS" psql -U $DB_USER -d $DB_NAME -h 127.0.0.1 -f "$SCRIPT_DIR/schema_reorg_support.sql"
    echo "✓ schema_reorg_support.sql applied"
else
    echo "⚠ schema_reorg_support.sql not found - skipping"
fi

echo ""
echo "========================================="
echo "Configuring PostgreSQL for TCP/IP"
echo "========================================="
echo ""

PG_HBA_FILE=$(sudo -u postgres psql -t -P format=unaligned -c 'show hba_file')
PG_CONF_FILE=$(sudo -u postgres psql -t -P format=unaligned -c 'show config_file')

echo "Configuration file: $PG_CONF_FILE"
echo "HBA file: $PG_HBA_FILE"
echo ""

# Check if listen_addresses is set
if ! sudo grep -q "^listen_addresses.*'localhost'" "$PG_CONF_FILE" 2>/dev/null; then
    echo "Configuring listen_addresses..."
    sudo sed -i "s/#listen_addresses = 'localhost'/listen_addresses = 'localhost'/" "$PG_CONF_FILE" || true
    echo "✓ listen_addresses configured"
fi

# Add authentication rule for local connections
if ! sudo grep -q "host.*$DB_NAME.*$DB_USER.*127.0.0.1/32.*md5" "$PG_HBA_FILE" 2>/dev/null; then
    echo "Adding authentication rule..."
    echo "host    $DB_NAME    $DB_USER    127.0.0.1/32    md5" | sudo tee -a "$PG_HBA_FILE" > /dev/null
    echo "✓ Authentication rule added"
fi

# Reload PostgreSQL configuration
echo "Reloading PostgreSQL configuration..."
sudo systemctl reload postgresql
sleep 1

echo "✓ PostgreSQL configured"
echo ""

# Test connection
echo "========================================="
echo "Testing Connection"
echo "========================================="
echo ""

if PGPASSWORD="$DB_PASS" psql -U $DB_USER -d $DB_NAME -h 127.0.0.1 -c "SELECT version();" > /dev/null 2>&1; then
    echo "✓ Connection test successful!"
else
    echo "✗ Connection test failed"
    echo ""
    echo "Try manually:"
    echo "  PGPASSWORD='$DB_PASS' psql -U $DB_USER -d $DB_NAME -h 127.0.0.1"
    exit 1
fi

echo ""
echo "========================================="
echo "Update inzyght.conf"
echo "========================================="
echo ""

if [ -f "$SCRIPT_DIR/inzyght.conf" ]; then
    echo "Current PostgreSQL configuration in inzyght.conf:"
    grep -A 5 '"postgresql"' "$SCRIPT_DIR/inzyght.conf" || echo "(not configured)"
    echo ""
    echo "Recommended configuration:"
    echo '  "postgresql": {'
    echo '    "host": "127.0.0.1",'
    echo '    "port": 5432,'
    echo "    \"database\": \"$DB_NAME\","
    echo "    \"username\": \"$DB_USER\","
    echo "    \"password\": \"$DB_PASS\""
    echo '  }'
else
    echo "⚠ inzyght.conf not found"
fi

echo ""
echo "========================================="
echo "Setup Complete!"
echo "========================================="
echo ""
echo "Database Details:"
echo "  Host:     127.0.0.1"
echo "  Port:     5432"
echo "  Database: $DB_NAME"
echo "  Username: $DB_USER"
echo "  Password: $DB_PASS"
echo ""
echo "Test manually:"
echo "  PGPASSWORD='$DB_PASS' psql -U $DB_USER -d $DB_NAME -h 127.0.0.1"
echo ""
echo "Start Inzyght:"
echo "  cd build && ./inzyght"
echo ""
